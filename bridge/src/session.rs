use crate::c2m::{
    encode_data_c2m_frame, handle_ack_c2m_with_rate, C2mSender,
};
use crate::config::{
    IO_BUF_LEN, SERIAL_RX_DRAIN_MAX_BYTES, SERIAL_RX_DRAIN_MAX_EVENTS, TCP_PENDING_LIMIT,
    TCP_READ_TIMEOUT, TCP_WRITE_TIMEOUT,
};
use crate::handshake::{normalize_ready, reset_link_and_wait_ready};
use crate::link;
use crate::log::{hex_preview, BridgeLog};
use crate::rate::RateController;
use crate::rate_calibration::calibrate_rate;
use crate::serial_io::{encode_frame, send_link_reset, write_data_frame_paced, write_frame};
use crate::serial_rx::{spawn_serial_rx_worker, SerialRxEvent, SerialRxWorker};
use serialport::SerialPort;
use std::io::{self, Read, Write};
use std::net::{TcpStream};
use std::sync::mpsc::{Receiver, TryRecvError};
use std::thread;
use std::time::{Duration, Instant};

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub(crate) enum SerialDrain {
    Continue,
    Reset,
}

pub(crate) fn small_packet_preview(direction: &str, payload: &[u8]) -> Option<String> {
    const MAX_SMALL_PACKET_BYTES: usize = 64;
    if payload.len() > MAX_SMALL_PACKET_BYTES {
        return None;
    }
    Some(format!(
        "{direction} tcp payload len={} data={}",
        payload.len(),
        hex_preview(payload)
    ))
}

pub(crate) fn accept_m2c_sequence(
    frame: &link::Frame,
    expected: &mut u16,
    _log: BridgeLog,
) -> io::Result<()> {
    if frame.seq != *expected {
        return Err(io::Error::new(
            io::ErrorKind::InvalidData,
            format!(
                "DATA_M2C sequence mismatch expected={} got={}",
                *expected, frame.seq
            ),
        ));
    }
    *expected = expected.wrapping_add(1);
    Ok(())
}

pub(crate) fn reset_c2m_sender(
    negotiated_payload: usize,
    credit: u16,
    sender: &mut C2mSender,
    m2c_seq_expected: &mut u16,
) {
    *sender = C2mSender::new(negotiated_payload, credit);
    *m2c_seq_expected = 0;
}

pub(crate) fn reset_link_session_state(
    serial: &mut dyn SerialPort,
    log: BridgeLog,
    negotiated_payload: &mut usize,
    credit: &mut u16,
    supported_rate_mask: &mut u16,
    rate_controller: &mut RateController,
    c2m_sender: &mut C2mSender,
    m2c_seq_expected: &mut u16,
) -> io::Result<()> {
    let ready = reset_link_and_wait_ready(serial, log)?;
    *negotiated_payload = ready.negotiated_payload;
    *credit = ready.initial_credit;
    *supported_rate_mask = ready.supported_rate_mask;
    *rate_controller = calibrate_rate(serial, log, *supported_rate_mask)?;
    reset_c2m_sender(*negotiated_payload, *credit, c2m_sender, m2c_seq_expected);
    Ok(())
}

pub(crate) fn reset_link_session_state_preserving_pending(
    serial: &mut dyn SerialPort,
    log: BridgeLog,
    negotiated_payload: &mut usize,
    credit: &mut u16,
    supported_rate_mask: &mut u16,
    rate_controller: &mut RateController,
    c2m_sender: &mut C2mSender,
    m2c_seq_expected: &mut u16,
) -> io::Result<()> {
    let ready = reset_link_and_wait_ready(serial, log)?;
    *negotiated_payload = ready.negotiated_payload;
    *credit = ready.initial_credit;
    *supported_rate_mask = ready.supported_rate_mask;
    *rate_controller = calibrate_rate(serial, log, *supported_rate_mask)?;
    c2m_sender.reset_link_state_preserving_pending(*negotiated_payload, *credit);
    *m2c_seq_expected = 0;
    Ok(())
}

pub(crate) fn handle_serial_frame(
    frame: link::Frame,
    tcp: &mut TcpStream,
    serial: &mut dyn SerialPort,
    log: BridgeLog,
    c2m_sender: &mut C2mSender,
    rate_controller: &mut RateController,
    credit: &mut u16,
    m2c_seq_expected: &mut u16,
    serial_to_tcp_total: &mut usize,
) -> io::Result<SerialDrain> {
    match frame.frame_type {
        link::FrameType::DataM2c => {
            accept_m2c_sequence(&frame, m2c_seq_expected, log)?;
            if let Err(e) = tcp.write_all(&frame.payload) {
                let _ = send_link_reset(serial, log);
                return Err(e);
            }
            if let Some(preview) = small_packet_preview("m2c", &frame.payload) {
                log.debug(preview);
            }
            *serial_to_tcp_total += frame.payload.len();
            log.debug(format_args!(
                "DATA_M2C wrote tcp bytes={} total={}",
                frame.payload.len(),
                *serial_to_tcp_total
            ));
        }
        link::FrameType::AckC2m => {
            handle_ack_c2m_with_rate(c2m_sender, rate_controller, &frame)?;
            *credit = c2m_sender.credit;
            log.debug(format_args!(
                "ACK_C2M ack={} credit={} pending={}",
                frame.ack,
                *credit,
                c2m_sender.pending_tcp_len()
            ));
        }
        link::FrameType::Ready => {
            if let Some(ready) = link::parse_ready(&frame.payload) {
                if let Some(ready_state) = normalize_ready(ready) {
                    log.info(format_args!(
                        "link ready update payload={} credit={} rate_mask=0x{:04x} flags=0x{:04x}",
                        ready_state.negotiated_payload,
                        ready_state.initial_credit,
                        ready_state.supported_rate_mask,
                        ready.flags
                    ));
                    return Ok(SerialDrain::Reset);
                } else {
                    log.info(format_args!(
                        "link READY with invalid values payload={} credit_cap={} credit={} rate_mask=0x{:04x} rate_profile={} flags=0x{:04x}",
                        ready.negotiated_payload,
                        ready.credit_cap,
                        ready.initial_credit,
                        ready.supported_rate_mask,
                        ready.initial_rate_profile,
                        ready.flags
                    ));
                }
            } else {
                log.info(format_args!(
                    "link READY with invalid payload {}",
                    hex_preview(&frame.payload)
                ));
            }
        }
        link::FrameType::ResetAck => {
            log.debug("link reset ack");
        }
        link::FrameType::Error => {
            log.info(format_args!(
                "link ERROR payload={}",
                hex_preview(&frame.payload)
            ));
            return Ok(SerialDrain::Reset);
        }
        link::FrameType::Ping => {
            let pong = encode_frame(link::FrameType::Pong, frame.seq, &frame.payload)?;
            write_frame(serial, &pong)?;
            log.debug("link ping -> pong");
        }
        link::FrameType::Unknown(raw) => {
            log.info(format_args!(
                "link unknown frame type=0x{raw:02x} seq={} payload={}",
                frame.seq,
                hex_preview(&frame.payload)
            ));
        }
        other => {
            log.debug(format_args!(
                "link ignored frame {other:?} seq={} payload={}",
                frame.seq,
                hex_preview(&frame.payload)
            ));
        }
    }

    Ok(SerialDrain::Continue)
}

pub(crate) fn handle_serial_rx_bytes(
    bytes: &[u8],
    decoder: &mut link::Decoder,
    tcp: &mut TcpStream,
    serial: &mut dyn SerialPort,
    log: BridgeLog,
    c2m_sender: &mut C2mSender,
    rate_controller: &mut RateController,
    credit: &mut u16,
    m2c_seq_expected: &mut u16,
    serial_to_tcp_total: &mut usize,
) -> io::Result<SerialDrain> {
    for event in decoder.feed(bytes) {
        let frame = match event {
            link::DecodeEvent::Frame(frame) => frame,
            link::DecodeEvent::Error(error) => {
                return Err(io::Error::new(
                    io::ErrorKind::InvalidData,
                    format!("serial link decode error: {error:?}"),
                ));
            }
        };

        if handle_serial_frame(
            frame,
            tcp,
            serial,
            log,
            c2m_sender,
            rate_controller,
            credit,
            m2c_seq_expected,
            serial_to_tcp_total,
        )? == SerialDrain::Reset
        {
            return Ok(SerialDrain::Reset);
        }
    }

    Ok(SerialDrain::Continue)
}

pub(crate) fn drain_serial_rx_events(
    rx: &Receiver<SerialRxEvent>,
    decoder: &mut link::Decoder,
    tcp: &mut TcpStream,
    serial: &mut dyn SerialPort,
    log: BridgeLog,
    c2m_sender: &mut C2mSender,
    rate_controller: &mut RateController,
    credit: &mut u16,
    m2c_seq_expected: &mut u16,
    serial_to_tcp_total: &mut usize,
) -> io::Result<SerialDrain> {
    let mut events_processed = 0usize;
    let mut bytes_processed = 0usize;

    loop {
        match rx.try_recv() {
            Ok(SerialRxEvent::Bytes(bytes)) => {
                events_processed += 1;
                bytes_processed += bytes.len();
                log.debug(format_args!("serial link read bytes={}", bytes.len()));
                if handle_serial_rx_bytes(
                    &bytes,
                    decoder,
                    tcp,
                    serial,
                    log,
                    c2m_sender,
                    rate_controller,
                    credit,
                    m2c_seq_expected,
                    serial_to_tcp_total,
                )? == SerialDrain::Reset
                {
                    return Ok(SerialDrain::Reset);
                }
                if events_processed >= SERIAL_RX_DRAIN_MAX_EVENTS
                    || bytes_processed >= SERIAL_RX_DRAIN_MAX_BYTES
                {
                    return Ok(SerialDrain::Continue);
                }
            }
            Ok(SerialRxEvent::Error(error)) => return Err(error),
            Err(TryRecvError::Empty | TryRecvError::Disconnected) => {
                return Ok(SerialDrain::Continue);
            }
        }
    }
}

pub(crate) fn reset_link_session_state_with_rx_worker(
    serial_rx: &mut SerialRxWorker,
    decoder: &mut link::Decoder,
    serial: &mut dyn SerialPort,
    log: BridgeLog,
    negotiated_payload: &mut usize,
    credit: &mut u16,
    supported_rate_mask: &mut u16,
    rate_controller: &mut RateController,
    c2m_sender: &mut C2mSender,
    m2c_seq_expected: &mut u16,
) -> io::Result<()> {
    // Stop the RX worker before synchronous reset/calibration reads so it cannot
    // consume reset READY or RATE_PROBE_ACK bytes.
    serial_rx.stop();
    reset_link_session_state(
        serial,
        log,
        negotiated_payload,
        credit,
        supported_rate_mask,
        rate_controller,
        c2m_sender,
        m2c_seq_expected,
    )?;
    *serial_rx = spawn_serial_rx_worker(serial)?;
    *decoder = link::Decoder::new();
    Ok(())
}

pub(crate) fn reset_link_session_state_preserving_pending_with_rx_worker(
    serial_rx: &mut SerialRxWorker,
    decoder: &mut link::Decoder,
    serial: &mut dyn SerialPort,
    log: BridgeLog,
    negotiated_payload: &mut usize,
    credit: &mut u16,
    supported_rate_mask: &mut u16,
    rate_controller: &mut RateController,
    c2m_sender: &mut C2mSender,
    m2c_seq_expected: &mut u16,
) -> io::Result<()> {
    // Stop the RX worker before synchronous reset/calibration reads so it cannot
    // consume reset READY or RATE_PROBE_ACK bytes.
    serial_rx.stop();
    reset_link_session_state_preserving_pending(
        serial,
        log,
        negotiated_payload,
        credit,
        supported_rate_mask,
        rate_controller,
        c2m_sender,
        m2c_seq_expected,
    )?;
    *serial_rx = spawn_serial_rx_worker(serial)?;
    *decoder = link::Decoder::new();
    Ok(())
}

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub(crate) enum RetransmitTimeoutAction {
    None,
    Retransmitted,
    Reset,
}

pub(crate) fn handle_retransmit_timeout(
    c2m_sender: &mut C2mSender,
    serial: &mut dyn SerialPort,
    log: BridgeLog,
    rate_controller: &mut RateController,
    retransmit_timeout: Duration,
    sleep: impl FnMut(Duration),
) -> io::Result<RetransmitTimeoutAction> {
    let Some(encoded) = c2m_sender.retransmit_if_due(Instant::now(), retransmit_timeout) else {
        return Ok(RetransmitTimeoutAction::None);
    };
    rate_controller.record_loss();
    if rate_controller.needs_recalibration() {
        return Ok(RetransmitTimeoutAction::Reset);
    }

    let profile = rate_controller.active_profile();
    write_data_frame_paced(serial, &encoded, profile, sleep)?;
    c2m_sender.mark_retransmitted(Instant::now());
    log.debug(format_args!(
        "DATA_C2M retransmit bytes={} sleep_us={}",
        encoded.len(),
        profile.inter_byte_sleep.as_micros()
    ));
    Ok(RetransmitTimeoutAction::Retransmitted)
}

pub(crate) fn configure_tcp_stream(tcp: &TcpStream) -> io::Result<()> {
    tcp.set_read_timeout(Some(TCP_READ_TIMEOUT))?;
    tcp.set_write_timeout(Some(TCP_WRITE_TIMEOUT))
}

pub(crate) fn run_link_client(
    mut tcp: TcpStream,
    serial: &mut dyn SerialPort,
    log: BridgeLog,
    c2m_retransmit_timeout: Duration,
) -> io::Result<()> {
    configure_tcp_stream(&tcp)?;

    let ready = reset_link_and_wait_ready(serial, log)?;
    let mut negotiated_payload = ready.negotiated_payload;
    let mut credit = ready.initial_credit;
    let mut supported_rate_mask = ready.supported_rate_mask;
    let mut rate_controller = calibrate_rate(serial, log, supported_rate_mask)?;
    let mut decoder = link::Decoder::new();
    let mut c2m_sender = C2mSender::new(negotiated_payload, credit);
    let mut m2c_seq_expected = 0u16;
    let mut tcp_buf = [0u8; IO_BUF_LEN];
    let mut tcp_read_total = 0usize;
    let mut tcp_to_serial_total = 0usize;
    let mut serial_to_tcp_total = 0usize;
    let mut serial_rx = spawn_serial_rx_worker(serial)?;

    loop {
        match drain_serial_rx_events(
            serial_rx.receiver(),
            &mut decoder,
            &mut tcp,
            serial,
            log,
            &mut c2m_sender,
            &mut rate_controller,
            &mut credit,
            &mut m2c_seq_expected,
            &mut serial_to_tcp_total,
        ) {
            Ok(SerialDrain::Continue) => {}
            Ok(SerialDrain::Reset) => {
                reset_link_session_state_with_rx_worker(
                    &mut serial_rx,
                    &mut decoder,
                    serial,
                    log,
                    &mut negotiated_payload,
                    &mut credit,
                    &mut supported_rate_mask,
                    &mut rate_controller,
                    &mut c2m_sender,
                    &mut m2c_seq_expected,
                )?;
                continue;
            }
            Err(e) => return Err(e),
        }

        if c2m_sender.pending_tcp_len() < TCP_PENDING_LIMIT {
            let read_cap = (TCP_PENDING_LIMIT - c2m_sender.pending_tcp_len()).min(tcp_buf.len());
            match tcp.read(&mut tcp_buf[..read_cap]) {
                Ok(0) => {
                    log.debug(format_args!("tcp eof total={tcp_to_serial_total}"));
                    let _ = send_link_reset(serial, log);
                    return Ok(());
                }
                Ok(n) => {
                    tcp_read_total += n;
                    let filter_stats = c2m_sender.push_filtered_tcp_bytes(&tcp_buf[..n]);
                    if let Some(preview) = small_packet_preview("c2m", &tcp_buf[..n]) {
                        log.debug(preview);
                    }
                    if filter_stats.dropped_movement_frames > 0 {
                        log.debug(format_args!(
                            "C2M filtered movement frames={} forwarded_bytes={} buffered_bytes={} pending={}",
                            filter_stats.dropped_movement_frames,
                            filter_stats.forwarded_bytes,
                            filter_stats.buffered_bytes,
                            c2m_sender.pending_tcp_len()
                        ));
                    }
                    log.debug(format_args!(
                        "tcp read bytes={n} pending={} total_in={}",
                        c2m_sender.pending_tcp_len(),
                        tcp_read_total
                    ));
                }
                Err(ref e)
                    if matches!(
                        e.kind(),
                        io::ErrorKind::WouldBlock | io::ErrorKind::TimedOut
                    ) => {}
                Err(e) => {
                    let _ = send_link_reset(serial, log);
                    return Err(e);
                }
            }
        }

        match handle_retransmit_timeout(
            &mut c2m_sender,
            serial,
            log,
            &mut rate_controller,
            c2m_retransmit_timeout,
            thread::sleep,
        ) {
            Ok(RetransmitTimeoutAction::Reset) => {
                if let Err(e) = reset_link_session_state_preserving_pending_with_rx_worker(
                    &mut serial_rx,
                    &mut decoder,
                    serial,
                    log,
                    &mut negotiated_payload,
                    &mut credit,
                    &mut supported_rate_mask,
                    &mut rate_controller,
                    &mut c2m_sender,
                    &mut m2c_seq_expected,
                ) {
                    let _ = send_link_reset(serial, log);
                    return Err(e);
                }
                continue;
            }
            Ok(RetransmitTimeoutAction::Retransmitted) => {}
            Err(e) => {
                let _ = send_link_reset(serial, log);
                return Err(e);
            }
            Ok(RetransmitTimeoutAction::None) => match c2m_sender.next_frame_to_send() {
                Ok(Some(frame)) => {
                    let encoded = encode_data_c2m_frame(&frame)?;
                    let payload_len = frame.payload.len();
                    let profile = rate_controller.active_profile();
                    if let Err(e) = write_data_frame_paced(serial, &encoded, profile, thread::sleep)
                    {
                        let _ = send_link_reset(serial, log);
                        return Err(e);
                    }
                    c2m_sender.mark_sent(&frame)?;
                    credit = c2m_sender.credit;
                    tcp_to_serial_total += payload_len;
                    log.debug(format_args!(
                        "DATA_C2M wrote bytes={payload_len} credit={credit} total={tcp_to_serial_total} sleep_us={}",
                        profile.inter_byte_sleep.as_micros()
                    ));
                }
                Ok(None) => {}
                Err(e) if e.kind() == io::ErrorKind::InvalidData => {
                    log.info(format_args!("DATA_C2M reset required: {e}"));
                    reset_link_session_state_preserving_pending_with_rx_worker(
                        &mut serial_rx,
                        &mut decoder,
                        serial,
                        log,
                        &mut negotiated_payload,
                        &mut credit,
                        &mut supported_rate_mask,
                        &mut rate_controller,
                        &mut c2m_sender,
                        &mut m2c_seq_expected,
                    )?;
                    thread::sleep(Duration::from_millis(1));
                    continue;
                }
                Err(e) => return Err(e),
            },
        }

        thread::sleep(Duration::from_millis(1));
    }
}
