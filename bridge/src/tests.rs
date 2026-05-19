use crate::c2m::{handle_ack_c2m, C2mSender};
use crate::config::{
    CONTROL_FRAME_WRITE_CHUNK_BYTES, CONTROL_FRAME_WRITE_DELAY, DEFAULT_BAUD,
    DEFAULT_C2M_RETRANSMIT_TIMEOUT_MS, SERIAL_RX_DRAIN_MAX_EVENTS, TCP_READ_TIMEOUT,
    TCP_WRITE_TIMEOUT,
};
use crate::link;
use crate::log::{hex_preview, BridgeLog};
use crate::rate::{self, RateController};
use crate::serial_io::{send_link_reset, write_data_frame_paced, write_frame, write_frame_paced};
use crate::serial_rx::{spawn_serial_rx_worker, SerialRxEvent};
use crate::rate_calibration::{calibrate_rate, calibrate_rate_with_sleep, rate_probe_payload};
use crate::handshake::{normalize_ready, reset_link_and_wait_ready_with_seq, wait_for_ready};
use crate::session::{
    accept_m2c_sequence, configure_tcp_stream, drain_serial_rx_events, handle_retransmit_timeout,
    reset_c2m_sender, run_link_client, RetransmitTimeoutAction, SerialDrain,
};
use crate::test_support::{FakeSerialPort, WriteAction};
use crate::Args;
use clap::Parser;
use std::io::{self, Read};
use std::net::{TcpListener, TcpStream};
use std::sync::mpsc::{self, TryRecvError};
use std::time::{Duration, Instant};

fn p0_rate_probe_acks() -> Vec<u8> {
    let mut readable = Vec::new();
    for probe_index in 0..5 {
        let payload = rate_probe_payload(rate::RATE_PROFILES[0], probe_index);
        readable.extend(
            link::encode(link::FrameType::RateProbeAck, probe_index, 0, &payload).unwrap(),
        );
    }
    readable
}

fn p0_calibration_write_len_after_reset() -> usize {
    let reset_len = link::encode(link::FrameType::Reset, 1, 0, &[])
        .unwrap()
        .len();
    let probe_len: usize = (0..5)
        .map(|probe_index| {
            let payload = rate_probe_payload(rate::RATE_PROFILES[0], probe_index);
            link::encode(link::FrameType::RateProbe, probe_index, 0, &payload)
                .unwrap()
                .len()
        })
        .sum();
    reset_len + probe_len
}

#[test]
fn default_baud_matches_firmware_default() {
    let args = Args::try_parse_from(["mc-uart-bridge", "--serial", "loop"]).unwrap();
    assert_eq!(args.baud, DEFAULT_BAUD);
}

#[test]
fn baud_can_be_overridden() {
    let args = Args::try_parse_from(["mc-uart-bridge", "--serial", "loop", "--baud", "921600"])
        .unwrap();
    assert_eq!(args.baud, 921600);
}

#[test]
fn verbose_defaults_to_false() {
    let args = Args::try_parse_from(["mc-uart-bridge", "--serial", "loop"]).unwrap();
    assert!(!args.verbose);
}

#[test]
fn verbose_can_be_enabled() {
    let args =
        Args::try_parse_from(["mc-uart-bridge", "--serial", "loop", "--verbose"]).unwrap();
    assert!(args.verbose);
}

#[test]
fn c2m_retransmit_timeout_defaults_to_one_second() {
    let args = Args::try_parse_from(["mc-uart-bridge", "--serial", "loop"]).unwrap();

    assert_eq!(
        args.c2m_retransmit_timeout_ms,
        DEFAULT_C2M_RETRANSMIT_TIMEOUT_MS
    );
}

#[test]
fn c2m_retransmit_timeout_can_be_overridden() {
    let args = Args::try_parse_from([
        "mc-uart-bridge",
        "--serial",
        "loop",
        "--c2m-retransmit-timeout-ms",
        "750",
    ])
    .unwrap();

    assert_eq!(args.c2m_retransmit_timeout_ms, 750);
}

#[test]
fn hex_preview_formats_short_buffers() {
    assert_eq!(hex_preview(&[0x0f, 0x00, 0x2f, 0xff]), "0f 00 2f ff");
}

#[test]
fn hex_preview_truncates_long_buffers() {
    let bytes: Vec<u8> = (0..70).collect();
    let preview = hex_preview(&bytes);
    assert!(preview.starts_with("00 01 02 03"));
    assert!(preview.ends_with(" ..."));
}

#[test]
fn c2m_sender_keeps_payload_until_ack_advances() {
    let mut sender = C2mSender::new(497, 512);
    sender.push_tcp_bytes(b"abc");
    let frame = sender.next_frame_to_send().unwrap().unwrap();
    assert_eq!(frame.seq, 0);
    assert_eq!(frame.payload, b"abc");
    assert_eq!(sender.pending_tcp_len(), 3);
    sender.mark_sent(&frame).unwrap();
    sender.handle_ack(1, 509).unwrap();
    assert_eq!(sender.pending_tcp_len(), 0);
}

#[test]
fn c2m_sender_duplicate_ack_updates_credit_without_dropping_outstanding() {
    let mut sender = C2mSender::new(497, 512);
    sender.push_tcp_bytes(b"abc");
    let frame = sender.next_frame_to_send().unwrap().unwrap();
    sender.mark_sent(&frame).unwrap();

    sender.handle_ack(0, 400).unwrap();

    assert_eq!(sender.pending_tcp_len(), 3);
    assert_eq!(sender.credit, 400);
    assert!(sender.outstanding.is_some());
    assert!(sender.next_frame_to_send().unwrap().is_none());
}

#[test]
fn c2m_sender_rejects_mark_sent_frame_drift() {
    let mut sender = C2mSender::new(497, 512);
    sender.push_tcp_bytes(b"abc");
    let mut frame = sender.next_frame_to_send().unwrap().unwrap();
    frame.seq = 1;

    let error = sender.mark_sent(&frame).unwrap_err();

    assert_eq!(error.kind(), io::ErrorKind::InvalidInput);
    assert!(sender.outstanding.is_none());
    assert_eq!(sender.pending_tcp_len(), 3);
}

#[test]
fn c2m_sender_refuses_reserved_max_sequence() {
    let mut sender = C2mSender::new(1, 512);
    sender.next_seq = u16::MAX;
    sender.push_tcp_bytes(b"a");

    let error = sender.next_frame_to_send().unwrap_err();

    assert_eq!(error.kind(), io::ErrorKind::InvalidData);
    assert_eq!(error.to_string(), "C2M sequence exhausted; reset required");
    assert_eq!(sender.pending_tcp_len(), 1);
    assert!(sender.outstanding.is_none());
}

#[test]
fn c2m_sender_ack_max_clears_sequence_before_reserved_max() {
    let mut sender = C2mSender::new(1, 512);
    sender.next_seq = u16::MAX - 1;
    sender.push_tcp_bytes(b"a");
    let frame = sender.next_frame_to_send().unwrap().unwrap();
    assert_eq!(frame.seq, u16::MAX - 1);
    sender.mark_sent(&frame).unwrap();

    sender.handle_ack(u16::MAX, 511).unwrap();

    assert_eq!(sender.pending_tcp_len(), 0);
    assert_eq!(sender.credit, 511);
    assert!(sender.outstanding.is_none());
}

#[test]
fn c2m_sender_unsolicited_zero_ack_does_not_clear_near_max_outstanding() {
    let mut sender = C2mSender::new(1, 512);
    sender.next_seq = u16::MAX - 1;
    sender.push_tcp_bytes(b"a");
    let frame = sender.next_frame_to_send().unwrap().unwrap();
    sender.mark_sent(&frame).unwrap();

    sender.handle_ack(0, 400).unwrap();

    assert_eq!(sender.pending_tcp_len(), 1);
    assert_eq!(sender.credit, 400);
    assert!(sender.outstanding.is_some());
}

#[test]
fn c2m_sender_sequence_exhaustion_reset_preserves_pending_tcp() {
    let mut sender = C2mSender::new(1, 512);
    sender.next_seq = u16::MAX;
    sender.push_tcp_bytes(b"abc");

    assert!(sender.next_frame_to_send().is_err());

    sender.reset_link_state_preserving_pending(128, 300);

    assert_eq!(sender.pending_tcp_len(), 3);
    assert_eq!(sender.negotiated_payload, 128);
    assert_eq!(sender.credit, 300);
    assert_eq!(sender.next_seq, 0);
    assert!(sender.outstanding.is_none());
    let frame = sender.next_frame_to_send().unwrap().unwrap();
    assert_eq!(frame.seq, 0);
    assert_eq!(frame.payload, b"abc");
}

#[test]
fn c2m_sender_reports_retransmit_due_without_touching_timing() {
    let mut sender = C2mSender::new(497, 512);
    sender.push_tcp_bytes(b"abc");
    let frame = sender.next_frame_to_send().unwrap().unwrap();
    let encoded = link::encode(frame.frame_type, frame.seq, frame.ack, &frame.payload).unwrap();
    sender.mark_sent(&frame).unwrap();

    let now = Instant::now();
    let timeout = Duration::from_millis(100);
    sender.outstanding.as_mut().unwrap().sent_at = now;

    assert!(sender
        .retransmit_if_due(now + Duration::from_millis(99), timeout)
        .is_none());
    assert_eq!(
        sender.retransmit_if_due(now + timeout, timeout),
        Some(encoded)
    );
    let outstanding = sender.outstanding.as_ref().unwrap();
    assert_eq!(outstanding.sent_at, now);
    assert_eq!(outstanding.retries, 0);
    assert_eq!(
        sender.retransmit_if_due(now + timeout, timeout),
        Some(link::encode(frame.frame_type, frame.seq, frame.ack, &frame.payload).unwrap())
    );
}

#[test]
fn c2m_sender_marks_retransmit_sent_after_write() {
    let mut sender = C2mSender::new(497, 512);
    sender.push_tcp_bytes(b"abc");
    let frame = sender.next_frame_to_send().unwrap().unwrap();
    sender.mark_sent(&frame).unwrap();
    let now = Instant::now();

    sender.mark_retransmitted(now);

    let outstanding = sender.outstanding.as_ref().unwrap();
    assert_eq!(outstanding.sent_at, now);
    assert_eq!(outstanding.retries, 1);
}

#[test]
fn ack_c2m_invalid_payload_errors() {
    let mut sender = C2mSender::new(497, 512);
    let frame = link::Frame {
        frame_type: link::FrameType::AckC2m,
        flags: 0,
        seq: 0,
        ack: 0,
        payload: vec![0x34],
    };

    let error = handle_ack_c2m(&mut sender, &frame).unwrap_err();

    assert_eq!(error.kind(), io::ErrorKind::InvalidData);
}

#[test]
fn normalize_ready_rejects_zero_payload_or_credit() {
    let mut ready = ready_payload(link::DEFAULT_PAYLOAD as u16, 512, 512, 0x001f, 0, 0);

    ready.negotiated_payload = 0;
    assert!(normalize_ready(ready).is_none());

    ready = ready_payload(link::DEFAULT_PAYLOAD as u16, 0, 512, 0x001f, 0, 0);
    assert!(normalize_ready(ready).is_none());

    ready = ready_payload(link::DEFAULT_PAYLOAD as u16, 512, 0, 0x001f, 0, 0);
    assert!(normalize_ready(ready).is_none());
}

#[test]
fn normalize_ready_rejects_initial_credit_above_credit_cap() {
    let ready = ready_payload(link::DEFAULT_PAYLOAD as u16, 511, 512, 0x001f, 0, 0);

    assert!(normalize_ready(ready).is_none());
}

#[test]
fn normalize_ready_rejects_non_p0_startup_profile() {
    let ready = ready_payload(link::DEFAULT_PAYLOAD as u16, 512, 512, 0x001f, 1, 0);

    assert!(normalize_ready(ready).is_none());
}

#[test]
fn normalize_ready_rejects_mask_without_p0() {
    let ready = ready_payload(link::DEFAULT_PAYLOAD as u16, 512, 512, 0x001e, 0, 0);

    assert!(normalize_ready(ready).is_none());
}

#[test]
fn c2m_sender_caps_frame_payload_to_available_credit_bytes() {
    let mut sender = C2mSender::new(64, 2);
    sender.push_tcp_bytes(b"abcdef");

    let frame = sender.next_frame_to_send().unwrap().unwrap();

    assert_eq!(frame.frame_type, link::FrameType::DataC2m);
    assert_eq!(frame.seq, 0);
    assert_eq!(frame.payload, b"ab");
    assert_eq!(sender.credit, 2);
    assert_eq!(sender.pending_tcp_len(), 6);
}

#[test]
fn c2m_sender_does_not_frame_when_credit_is_zero() {
    let mut sender = C2mSender::new(64, 0);
    sender.push_tcp_bytes(b"abc");

    assert!(sender.next_frame_to_send().unwrap().is_none());
    assert_eq!(sender.pending_tcp_len(), 3);
}

#[test]
fn reset_c2m_sender_restarts_sequence_and_clears_pending_tcp() {
    let mut sender = C2mSender::new(32, 4);
    sender.next_seq = 9;
    sender.push_tcp_bytes(b"abc");
    let mut m2c_seq_expected = 7u16;

    reset_c2m_sender(128, 300, &mut sender, &mut m2c_seq_expected);

    assert_eq!(sender.negotiated_payload, 128);
    assert_eq!(sender.credit, 300);
    assert_eq!(sender.next_seq, 0);
    assert!(sender.outstanding.is_none());
    assert!(sender.pending_tcp.is_empty());
    assert_eq!(m2c_seq_expected, 0);
}

#[test]
fn m2c_sequence_mismatch_is_invalid_data() {
    let log = BridgeLog { verbose: false };
    let mut expected = 2u16;
    let frame = link::Frame {
        frame_type: link::FrameType::DataM2c,
        flags: 0,
        seq: 1,
        ack: 0,
        payload: b"stale".to_vec(),
    };

    let err = accept_m2c_sequence(&frame, &mut expected, log).unwrap_err();

    assert_eq!(err.kind(), io::ErrorKind::InvalidData);
    assert_eq!(expected, 2);
}

#[test]
fn m2c_sequence_match_returns_ok_and_advances_expected() {
    let log = BridgeLog { verbose: false };
    let mut expected = u16::MAX;
    let frame = link::Frame {
        frame_type: link::FrameType::DataM2c,
        flags: 0,
        seq: u16::MAX,
        ack: 0,
        payload: b"ok".to_vec(),
    };

    accept_m2c_sequence(&frame, &mut expected, log).unwrap();

    assert_eq!(expected, 0);
}

#[test]
fn configure_tcp_stream_sets_read_and_write_timeouts() {
    let listener = TcpListener::bind("127.0.0.1:0").unwrap();
    let addr = listener.local_addr().unwrap();
    let client = TcpStream::connect(addr).unwrap();
    let (server, _) = listener.accept().unwrap();

    configure_tcp_stream(&server).unwrap();

    let read_timeout = server.read_timeout().unwrap().unwrap();
    let write_timeout = server.write_timeout().unwrap().unwrap();
    assert!(read_timeout >= TCP_READ_TIMEOUT);
    assert!(read_timeout <= Duration::from_millis(20));
    assert!(write_timeout >= TCP_WRITE_TIMEOUT);
    assert!(write_timeout <= TCP_WRITE_TIMEOUT + Duration::from_millis(20));
    drop(client);
}

#[test]
fn write_frame_retries_timeout_after_partial_progress() {
    let frame = link::encode(link::FrameType::Ping, 4, 0, b"abc").unwrap();
    let mut serial = FakeSerialPort::new([
        WriteAction::Write(2),
        WriteAction::Error(io::ErrorKind::TimedOut),
        WriteAction::Write(usize::MAX),
    ]);

    write_frame(&mut serial, &frame).unwrap();

    assert_eq!(serial.written, frame);
}

#[test]
fn control_frame_write_paces_one_byte_at_a_time() {
    use std::cell::RefCell;

    let frame = link::encode(
        link::FrameType::Hello,
        0,
        0,
        &link::hello_payload(link::DEFAULT_PAYLOAD as u16, link::INITIAL_CREDIT),
    )
    .unwrap();
    let mut serial = FakeSerialPort::new(frame.iter().map(|_| WriteAction::Write(1)));
    let sleeps = RefCell::new(Vec::new());

    write_frame_paced(
        &mut serial,
        &frame,
        CONTROL_FRAME_WRITE_CHUNK_BYTES,
        CONTROL_FRAME_WRITE_DELAY,
        |delay| sleeps.borrow_mut().push(delay),
    )
    .unwrap();

    assert_eq!(serial.written, frame);
    assert_eq!(sleeps.into_inner().len(), frame.len() - 1);
}

#[test]
fn data_frame_write_uses_selected_rate_profile() {
    use std::cell::RefCell;

    let frame = link::encode(link::FrameType::DataC2m, 0, 0, b"login").unwrap();
    let profile = rate::RATE_PROFILES[2];
    let mut serial = FakeSerialPort::new([]);
    let sleeps = RefCell::new(Vec::new());

    write_data_frame_paced(&mut serial, &frame, profile, |delay| {
        sleeps.borrow_mut().push(delay)
    })
    .unwrap();

    assert_eq!(serial.written, frame);
    let sleeps = sleeps.into_inner();
    assert_eq!(sleeps.len(), frame.len() - 1);
    assert!(sleeps
        .iter()
        .all(|delay| *delay == profile.inter_byte_sleep));
}

#[test]
fn send_link_reset_writes_reset_frame() {
    let frame = link::encode(link::FrameType::Reset, 0, 0, &[]).unwrap();
    let mut serial = FakeSerialPort::new(frame.iter().map(|_| WriteAction::Write(1)));
    let log = BridgeLog { verbose: false };

    send_link_reset(&mut serial, log).unwrap();

    let mut decoder = link::Decoder::new();
    let events = decoder.feed(&serial.written);
    assert_eq!(events.len(), 1);
    let link::DecodeEvent::Frame(frame) = &events[0] else {
        panic!("expected frame event");
    };
    assert_eq!(frame.frame_type, link::FrameType::Reset);
    assert!(frame.payload.is_empty());
}

#[test]
fn wait_for_ready_accepts_ready_frame() {
    let ready = link::encode(
        link::FrameType::Ready,
        0,
        0,
        &[
            0xf1, 0x01, 0x00, 0x02, 0xf0, 0x01, 0x1f, 0x00, 0x00, 0x00, 0x00,
        ],
    )
    .unwrap();
    let mut serial = FakeSerialPort::new([]).with_readable(ready);
    let log = BridgeLog { verbose: false };

    let ready = wait_for_ready(&mut serial, log).unwrap();

    assert_eq!(ready.negotiated_payload, link::DEFAULT_PAYLOAD);
    assert_eq!(ready.initial_credit, 496);
    assert_eq!(ready.supported_rate_mask, 0x001f);
    assert_eq!(serial.written.last().copied(), Some(link::DELIMITER));
}

#[test]
fn reset_link_waits_for_reset_ack_before_accepting_ready() {
    let stale_ready = link::encode(
        link::FrameType::Ready,
        0,
        0,
        &[
            0x20, 0x00, 0x00, 0x02, 0x10, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00,
        ],
    )
    .unwrap();
    let reset_seq = 0x1234u16;
    let reset_ack = link::encode(link::FrameType::ResetAck, reset_seq, 0, &[]).unwrap();
    let fresh_ready = link::encode(
        link::FrameType::Ready,
        0,
        0,
        &[
            0xf1, 0x01, 0x00, 0x02, 0xf0, 0x01, 0x1f, 0x00, 0x00, 0x00, 0x00,
        ],
    )
    .unwrap();
    let mut readable = stale_ready;
    readable.extend(reset_ack);
    readable.extend(fresh_ready);
    let mut serial = FakeSerialPort::new([]).with_readable(readable);
    let log = BridgeLog { verbose: false };

    let ready = reset_link_and_wait_ready_with_seq(&mut serial, log, reset_seq, |_| {}).unwrap();

    assert_eq!(ready.negotiated_payload, link::DEFAULT_PAYLOAD);
    assert_eq!(ready.initial_credit, 496);
    assert_eq!(ready.supported_rate_mask, 0x001f);
}

#[test]
fn reset_link_does_not_retransmit_while_ack_bytes_arrive_slowly() {
    let reset_seq = 0x1234u16;
    let reset_ack = link::encode(link::FrameType::ResetAck, reset_seq, 0, &[]).unwrap();
    let ready = link::encode(
        link::FrameType::Ready,
        0,
        0,
        &[
            0xf1, 0x01, 0x00, 0x02, 0xf0, 0x01, 0x1f, 0x00, 0x00, 0x00, 0x00,
        ],
    )
    .unwrap();
    let mut readable = reset_ack;
    readable.extend(ready);
    let mut serial = FakeSerialPort::new([])
        .with_readable(readable)
        .with_max_read_len(1)
        .with_read_delay(Duration::from_millis(15));
    let log = BridgeLog { verbose: false };

    reset_link_and_wait_ready_with_seq(&mut serial, log, reset_seq, |_| {}).unwrap();

    let mut decoder = link::Decoder::new();
    let resets = decoder
        .feed(&serial.written)
        .into_iter()
        .filter(|event| {
            matches!(
                event,
                link::DecodeEvent::Frame(link::Frame {
                    frame_type: link::FrameType::Reset,
                    ..
                })
            )
        })
        .count();
    assert_eq!(resets, 1);
}

#[test]
fn drain_serial_rx_events_decodes_split_m2c_frame() {
    let (tx, rx) = mpsc::channel();
    let encoded = link::encode(link::FrameType::DataM2c, 0, 0, b"abc").unwrap();
    tx.send(SerialRxEvent::Bytes(encoded[..2].to_vec())).unwrap();
    tx.send(SerialRxEvent::Bytes(encoded[2..].to_vec())).unwrap();
    drop(tx);

    let listener = TcpListener::bind("127.0.0.1:0").unwrap();
    let addr = listener.local_addr().unwrap();
    let mut client = TcpStream::connect(addr).unwrap();
    client
        .set_read_timeout(Some(Duration::from_secs(1)))
        .unwrap();
    let (mut server, _) = listener.accept().unwrap();
    configure_tcp_stream(&server).unwrap();

    let mut decoder = link::Decoder::new();
    let mut sender = C2mSender::new(497, 512);
    let mut rate_controller = RateController::new();
    let mut m2c_seq_expected = 0u16;
    let mut serial_to_tcp_total = 0usize;
    let mut serial = FakeSerialPort::new([]);
    let log = BridgeLog { verbose: false };

    let drain = drain_serial_rx_events(
        &rx,
        &mut decoder,
        &mut server,
        &mut serial,
        log,
        &mut sender,
        &mut rate_controller,
        &mut 512u16,
        &mut m2c_seq_expected,
        &mut serial_to_tcp_total,
    )
    .unwrap();

    assert_eq!(drain, SerialDrain::Continue);
    assert_eq!(serial_to_tcp_total, 3);
    let mut out = [0u8; 3];
    client.read_exact(&mut out).unwrap();
    assert_eq!(&out, b"abc");
}

#[test]
fn drain_serial_rx_events_surfaces_rx_worker_error() {
    let (tx, rx) = mpsc::channel();
    tx.send(SerialRxEvent::Error(io::Error::new(
        io::ErrorKind::BrokenPipe,
        "serial rx failed",
    )))
    .unwrap();
    drop(tx);

    let listener = TcpListener::bind("127.0.0.1:0").unwrap();
    let addr = listener.local_addr().unwrap();
    let _client = TcpStream::connect(addr).unwrap();
    let (mut server, _) = listener.accept().unwrap();
    configure_tcp_stream(&server).unwrap();

    let mut decoder = link::Decoder::new();
    let mut sender = C2mSender::new(497, 512);
    let mut rate_controller = RateController::new();
    let mut m2c_seq_expected = 0u16;
    let mut serial_to_tcp_total = 0usize;
    let mut serial = FakeSerialPort::new([]);
    let log = BridgeLog { verbose: false };

    let err = drain_serial_rx_events(
        &rx,
        &mut decoder,
        &mut server,
        &mut serial,
        log,
        &mut sender,
        &mut rate_controller,
        &mut 512u16,
        &mut m2c_seq_expected,
        &mut serial_to_tcp_total,
    )
    .unwrap_err();

    assert_eq!(err.kind(), io::ErrorKind::BrokenPipe);
    assert_eq!(err.to_string(), "serial rx failed");
}

#[test]
fn spawn_serial_rx_worker_returns_clone_error() {
    let mut serial = FakeSerialPort::new([]).with_clone_error(io::ErrorKind::Other);
    let err = match spawn_serial_rx_worker(&mut serial) {
        Ok(_) => panic!("expected clone error"),
        Err(err) => err,
    };

    assert_eq!(err.kind(), io::ErrorKind::Other);
    assert!(err.to_string().contains("clone failed"));
}

#[test]
fn serial_rx_worker_forwards_read_bytes() {
    let readable = link::encode(link::FrameType::DataM2c, 0, 0, b"rx").unwrap();
    let mut serial = FakeSerialPort::new([]).with_readable(readable.clone());
    let mut worker = spawn_serial_rx_worker(&mut serial).unwrap();

    let event = worker
        .receiver()
        .recv_timeout(Duration::from_secs(1))
        .unwrap();
    match event {
        SerialRxEvent::Bytes(bytes) => assert_eq!(bytes, readable),
        SerialRxEvent::Error(error) => panic!("unexpected rx error: {error}"),
    }
    worker.stop();
}

#[test]
fn serial_rx_worker_stops_and_joins_on_request() {
    let mut serial = FakeSerialPort::new([]).with_empty_read_error(io::ErrorKind::TimedOut);
    let mut worker = spawn_serial_rx_worker(&mut serial).unwrap();

    worker.stop();

    assert!(matches!(
        worker.receiver().try_recv(),
        Err(TryRecvError::Disconnected)
    ));
}

#[test]
fn drain_serial_rx_events_event_budget_defers_later_m2c_frame() {
    let (tx, rx) = mpsc::channel();
    for seq in 0..SERIAL_RX_DRAIN_MAX_EVENTS {
        tx.send(SerialRxEvent::Bytes(
            link::encode(link::FrameType::ResetAck, seq as u16, 0, &[]).unwrap(),
        ))
        .unwrap();
    }
    tx.send(SerialRxEvent::Bytes(
        link::encode(link::FrameType::DataM2c, 0, 0, b"abc").unwrap(),
    ))
    .unwrap();
    drop(tx);

    let listener = TcpListener::bind("127.0.0.1:0").unwrap();
    let addr = listener.local_addr().unwrap();
    let mut client = TcpStream::connect(addr).unwrap();
    client
        .set_read_timeout(Some(Duration::from_secs(1)))
        .unwrap();
    let (mut server, _) = listener.accept().unwrap();
    configure_tcp_stream(&server).unwrap();

    let mut decoder = link::Decoder::new();
    let mut sender = C2mSender::new(497, 512);
    let mut rate_controller = RateController::new();
    let mut m2c_seq_expected = 0u16;
    let mut serial_to_tcp_total = 0usize;
    let mut serial = FakeSerialPort::new([]);
    let log = BridgeLog { verbose: false };

    let drain = drain_serial_rx_events(
        &rx,
        &mut decoder,
        &mut server,
        &mut serial,
        log,
        &mut sender,
        &mut rate_controller,
        &mut 512u16,
        &mut m2c_seq_expected,
        &mut serial_to_tcp_total,
    )
    .unwrap();

    assert_eq!(drain, SerialDrain::Continue);
    assert_eq!(serial_to_tcp_total, 0);

    let drain = drain_serial_rx_events(
        &rx,
        &mut decoder,
        &mut server,
        &mut serial,
        log,
        &mut sender,
        &mut rate_controller,
        &mut 512u16,
        &mut m2c_seq_expected,
        &mut serial_to_tcp_total,
    )
    .unwrap();

    assert_eq!(drain, SerialDrain::Continue);
    assert_eq!(serial_to_tcp_total, 3);
    let mut out = [0u8; 3];
    client.read_exact(&mut out).unwrap();
    assert_eq!(&out, b"abc");
}

#[test]
fn drain_serial_rx_events_valid_ready_returns_reset_without_sync_reset() {
    let ready_payload = [
        0xf1, 0x01, 0x00, 0x02, 0xf0, 0x01, 0x01, 0x00, 0x00, 0x00, 0x00,
    ];
    let (tx, rx) = mpsc::channel();
    tx.send(SerialRxEvent::Bytes(
        link::encode(link::FrameType::Ready, 0, 0, &ready_payload).unwrap(),
    ))
    .unwrap();
    drop(tx);

    let listener = TcpListener::bind("127.0.0.1:0").unwrap();
    let addr = listener.local_addr().unwrap();
    let _client = TcpStream::connect(addr).unwrap();
    let (mut server, _) = listener.accept().unwrap();
    configure_tcp_stream(&server).unwrap();

    let mut decoder = link::Decoder::new();
    let mut sender = C2mSender::new(497, 512);
    let mut rate_controller = RateController::new();
    let mut m2c_seq_expected = 0u16;
    let mut serial_to_tcp_total = 0usize;
    let mut serial = FakeSerialPort::new([]).with_empty_read_error(io::ErrorKind::BrokenPipe);
    let log = BridgeLog { verbose: false };

    let drain = drain_serial_rx_events(
        &rx,
        &mut decoder,
        &mut server,
        &mut serial,
        log,
        &mut sender,
        &mut rate_controller,
        &mut 512u16,
        &mut m2c_seq_expected,
        &mut serial_to_tcp_total,
    )
    .unwrap();

    assert_eq!(drain, SerialDrain::Reset);
    assert!(serial.written.is_empty());
}

#[test]
fn run_link_client_returns_invalid_data_on_serial_decode_error() {
    let ready_payload = [
        0xf1, 0x01, 0x00, 0x02, 0xf0, 0x01, 0x01, 0x00, 0x00, 0x00, 0x00,
    ];
    let mut corrupt = link::encode(link::FrameType::DataM2c, 0, 0, b"bad").unwrap();
    let last_data = corrupt.len() - 2;
    corrupt[last_data] ^= 0x20;
    let mut serial = FakeSerialPort::new([])
        .with_readable(p0_rate_probe_acks())
        .with_readable_after_written_bytes(p0_calibration_write_len_after_reset(), corrupt)
        .with_auto_reset_ready(&ready_payload)
        .with_empty_read_error(io::ErrorKind::BrokenPipe);
    let listener = TcpListener::bind("127.0.0.1:0").unwrap();
    let addr = listener.local_addr().unwrap();
    let _client = TcpStream::connect(addr).unwrap();
    let (server, _) = listener.accept().unwrap();
    let log = BridgeLog { verbose: false };

    let err = run_link_client(
        server,
        &mut serial,
        log,
        Duration::from_millis(DEFAULT_C2M_RETRANSMIT_TIMEOUT_MS),
    )
    .unwrap_err();

    assert_eq!(err.kind(), io::ErrorKind::InvalidData);
}

#[test]
fn run_link_client_returns_invalid_data_on_m2c_sequence_mismatch() {
    let ready_payload = [
        0xf1, 0x01, 0x00, 0x02, 0xf0, 0x01, 0x01, 0x00, 0x00, 0x00, 0x00,
    ];
    let stale = link::encode(link::FrameType::DataM2c, 1, 0, b"stale").unwrap();
    let mut serial = FakeSerialPort::new([])
        .with_readable(p0_rate_probe_acks())
        .with_readable_after_written_bytes(p0_calibration_write_len_after_reset(), stale)
        .with_auto_reset_ready(&ready_payload)
        .with_empty_read_error(io::ErrorKind::BrokenPipe);
    let listener = TcpListener::bind("127.0.0.1:0").unwrap();
    let addr = listener.local_addr().unwrap();
    let _client = TcpStream::connect(addr).unwrap();
    let (server, _) = listener.accept().unwrap();
    let log = BridgeLog { verbose: false };

    let err = run_link_client(
        server,
        &mut serial,
        log,
        Duration::from_millis(DEFAULT_C2M_RETRANSMIT_TIMEOUT_MS),
    )
    .unwrap_err();

    assert_eq!(err.kind(), io::ErrorKind::InvalidData);
    assert_eq!(
        err.to_string(),
        "DATA_M2C sequence mismatch expected=0 got=1"
    );
}

#[test]
fn calibrate_rate_marks_highest_profile_after_three_successes_per_profile() {
    use std::cell::RefCell;

    let log = BridgeLog { verbose: false };
    let mut readable = Vec::new();
    for probe_index in 0..5 {
        let payload = rate_probe_payload(rate::RATE_PROFILES[0], probe_index);
        readable.extend(
            link::encode(link::FrameType::RateProbeAck, probe_index, 0, &payload).unwrap(),
        );
    }
    for probe_index in 5..8 {
        let payload = rate_probe_payload(rate::RATE_PROFILES[1], probe_index);
        readable.extend(
            link::encode(link::FrameType::RateProbeAck, probe_index, 0, &payload).unwrap(),
        );
    }
    for probe_index in 8..11 {
        let payload = rate_probe_payload(rate::RATE_PROFILES[2], probe_index);
        readable.extend(
            link::encode(link::FrameType::RateProbeAck, probe_index, 0, &payload).unwrap(),
        );
    }
    let first_ack_len = link::encode(
        link::FrameType::RateProbeAck,
        0,
        0,
        &rate_probe_payload(rate::RATE_PROFILES[0], 0),
    )
    .unwrap()
    .len();
    let mut serial = FakeSerialPort::new([])
        .with_readable(readable)
        .with_max_read_len(first_ack_len);
    let sleeps = RefCell::new(Vec::new());

    let controller = calibrate_rate_with_sleep(&mut serial, log, 0x0007, |delay| {
        sleeps.borrow_mut().push(delay)
    })
    .unwrap();

    assert_eq!(controller.active_profile().id, 1);
    let mut decoder = link::Decoder::new();
    let frames: Vec<_> = decoder
        .feed(&serial.written)
        .into_iter()
        .map(|event| match event {
            link::DecodeEvent::Frame(frame) => frame,
            link::DecodeEvent::Error(error) => panic!("decode error: {error:?}"),
        })
        .collect();
    assert_eq!(frames.len(), 11);
    assert!(frames
        .iter()
        .all(|frame| frame.frame_type == link::FrameType::RateProbe));
    assert_eq!(
        frames[2].payload,
        rate_probe_payload(rate::RATE_PROFILES[0], 2)
    );
    assert_eq!(
        frames[5].payload,
        rate_probe_payload(rate::RATE_PROFILES[1], 5)
    );
    assert_eq!(
        frames[8].payload,
        rate_probe_payload(rate::RATE_PROFILES[2], 8)
    );
    let sleeps = sleeps.into_inner();
    let first_probe_len =
        link::encode(link::FrameType::RateProbe, 0, 0, &frames[0].payload)
            .unwrap()
            .len();
    let expected_probe_sleeps = first_probe_len - 1;
    assert_eq!(sleeps.len(), expected_probe_sleeps * frames.len());
    assert!(sleeps[..expected_probe_sleeps * 5]
        .iter()
        .all(|delay| *delay == rate::RATE_PROFILES[0].inter_byte_sleep));
    assert!(sleeps[expected_probe_sleeps * 5..expected_probe_sleeps * 8]
        .iter()
        .all(|delay| *delay == rate::RATE_PROFILES[1].inter_byte_sleep));
    assert!(sleeps[expected_probe_sleeps * 8..]
        .iter()
        .all(|delay| *delay == rate::RATE_PROFILES[2].inter_byte_sleep));
}

#[test]
fn calibrate_rate_uses_supported_profile_below_highest_stable() {
    let log = BridgeLog { verbose: false };
    let mut readable = Vec::new();
    for probe_index in 0..5 {
        let payload = rate_probe_payload(rate::RATE_PROFILES[0], probe_index);
        readable.extend(
            link::encode(link::FrameType::RateProbeAck, probe_index, 0, &payload).unwrap(),
        );
    }
    for probe_index in 5..8 {
        let payload = rate_probe_payload(rate::RATE_PROFILES[2], probe_index);
        readable.extend(
            link::encode(link::FrameType::RateProbeAck, probe_index, 0, &payload).unwrap(),
        );
    }
    let first_ack_len = link::encode(
        link::FrameType::RateProbeAck,
        0,
        0,
        &rate_probe_payload(rate::RATE_PROFILES[0], 0),
    )
    .unwrap()
    .len();
    let mut serial = FakeSerialPort::new([])
        .with_readable(readable)
        .with_max_read_len(first_ack_len);

    let controller = calibrate_rate_with_sleep(&mut serial, log, 0x0005, |_| {}).unwrap();

    assert_eq!(controller.active_profile().id, 0);
}

#[test]
fn calibrate_rate_stops_on_borderline_profile_and_uses_one_slower() {
    let log = BridgeLog { verbose: false };
    let mut readable = Vec::new();
    for probe_index in [0u16, 1, 2, 3, 4, 5, 7] {
        let profile = if probe_index < 5 {
            rate::RATE_PROFILES[0]
        } else {
            rate::RATE_PROFILES[1]
        };
        let payload = rate_probe_payload(profile, probe_index);
        readable.extend(
            link::encode(link::FrameType::RateProbeAck, probe_index, 0, &payload).unwrap(),
        );
    }
    let mut serial = FakeSerialPort::new([])
        .with_readable(readable)
        .with_max_read_len(usize::MAX)
        .with_empty_read_error(io::ErrorKind::TimedOut);

    let controller = calibrate_rate_with_sleep(&mut serial, log, 0x0003, |_| {}).unwrap();

    assert_eq!(controller.active_profile().id, 0);
    let mut decoder = link::Decoder::new();
    let frames: Vec<_> = decoder
        .feed(&serial.written)
        .into_iter()
        .map(|event| match event {
            link::DecodeEvent::Frame(frame) => frame,
            link::DecodeEvent::Error(error) => panic!("decode error: {error:?}"),
        })
        .collect();
    assert_eq!(frames.len(), 8);
    assert_eq!(
        frames[5].payload,
        rate_probe_payload(rate::RATE_PROFILES[1], 5)
    );
    assert_eq!(
        frames[7].payload,
        rate_probe_payload(rate::RATE_PROFILES[1], 7)
    );
}

#[test]
fn calibrate_rate_errors_when_slowest_profile_has_no_successes() {
    let log = BridgeLog { verbose: false };
    let mut serial = FakeSerialPort::new([]);

    let error = match calibrate_rate(&mut serial, log, 0x0001) {
        Ok(_) => panic!("expected slowest profile calibration to fail"),
        Err(error) => error,
    };

    assert_eq!(error.kind(), io::ErrorKind::TimedOut);
    assert!(!serial.written.is_empty());
}

#[test]
fn calibrate_rate_rejects_matching_payload_with_wrong_ack_seq() {
    let log = BridgeLog { verbose: false };
    let mut readable = Vec::new();
    for probe_index in 0..5 {
        let payload = rate_probe_payload(rate::RATE_PROFILES[0], probe_index);
        readable.extend(
            link::encode(link::FrameType::RateProbeAck, probe_index, 0, &payload).unwrap(),
        );
    }
    for probe_index in 5..8 {
        let payload = rate_probe_payload(rate::RATE_PROFILES[1], probe_index);
        readable.extend(
            link::encode(link::FrameType::RateProbeAck, probe_index, 0, &payload).unwrap(),
        );
    }
    for probe_index in 8..11 {
        let payload = rate_probe_payload(rate::RATE_PROFILES[2], probe_index);
        readable.extend(link::encode(link::FrameType::RateProbeAck, 99, 0, &payload).unwrap());
    }
    let first_ack_len = link::encode(
        link::FrameType::RateProbeAck,
        0,
        0,
        &rate_probe_payload(rate::RATE_PROFILES[0], 0),
    )
    .unwrap()
    .len();
    let mut serial = FakeSerialPort::new([])
        .with_readable(readable)
        .with_max_read_len(first_ack_len);

    let controller = calibrate_rate_with_sleep(&mut serial, log, 0x0007, |_| {}).unwrap();

    assert_eq!(controller.active_profile().id, 0);
}

#[test]
fn retransmit_recalibration_requests_reset_without_sync_serial_reset() {
    let mut sender = C2mSender::new(497, 512);
    sender.push_tcp_bytes(b"abc");
    let frame = sender.next_frame_to_send().unwrap().unwrap();
    sender.mark_sent(&frame).unwrap();
    sender.outstanding.as_mut().unwrap().sent_at =
        Instant::now()
            - Duration::from_millis(DEFAULT_C2M_RETRANSMIT_TIMEOUT_MS)
            - Duration::from_millis(1);
    let mut controller = RateController::new();
    controller.mark_profile_stable(3);
    for _ in 0..6 {
        controller.record_loss();
    }
    let mut serial = FakeSerialPort::new([])
        .with_readable(p0_rate_probe_acks())
        .with_auto_reset_ready(&[
            0xf1, 0x01, 0x00, 0x02, 0xf0, 0x01, 0x01, 0x00, 0x00, 0x00, 0x00,
        ]);
    let log = BridgeLog { verbose: false };

    assert_eq!(
        handle_retransmit_timeout(
            &mut sender,
            &mut serial,
            log,
            &mut controller,
            Duration::from_millis(DEFAULT_C2M_RETRANSMIT_TIMEOUT_MS),
            |_| {}
        )
        .unwrap(),
        RetransmitTimeoutAction::Reset
    );

    assert_eq!(sender.pending_tcp_len(), 3);
    assert!(sender.outstanding.is_some());
    assert_eq!(sender.next_seq, 1);
    assert!(serial.written.is_empty());
}

fn ready_payload(
    negotiated_payload: u16,
    credit_cap: u16,
    initial_credit: u16,
    supported_rate_mask: u16,
    initial_rate_profile: u8,
    flags: u16,
) -> link::ReadyPayload {
    link::ReadyPayload {
        negotiated_payload,
        credit_cap,
        initial_credit,
        supported_rate_mask,
        initial_rate_profile,
        flags,
    }
}
