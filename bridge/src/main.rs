mod link;
mod rate;

use clap::Parser;
use rate::RateController;
use serialport::SerialPort;
use std::collections::VecDeque;
use std::io::{self, Read, Write};
use std::net::{TcpListener, TcpStream};
use std::sync::atomic::{AtomicU16, Ordering};
use std::thread;
use std::time::{Duration, Instant};

const DEFAULT_BAUD: u32 = 115200;
const TCP_PENDING_LIMIT: usize = 8192;
const IO_BUF_LEN: usize = 8192;
const TCP_READ_TIMEOUT: Duration = Duration::from_millis(1);
const TCP_WRITE_TIMEOUT: Duration = Duration::from_secs(1);
const SERIAL_WRITE_BACKPRESSURE_LIMIT: Duration = Duration::from_secs(1);
const LINK_READY_TIMEOUT: Duration = Duration::from_secs(5);
const LINK_HELLO_INTERVAL: Duration = Duration::from_millis(100);
const CONTROL_FRAME_WRITE_CHUNK_BYTES: usize = 1;
const CONTROL_FRAME_WRITE_DELAY: Duration = Duration::from_millis(10);
const C2M_RETRANSMIT_TIMEOUT: Duration = Duration::from_millis(250);
#[cfg(not(test))]
const RATE_CALIBRATION_SETTLE_DELAY: Duration = Duration::from_millis(50);
#[cfg(test)]
const RATE_CALIBRATION_SETTLE_DELAY: Duration = Duration::ZERO;
#[cfg(not(test))]
const RATE_PROBE_ACK_TIMEOUT: Duration = Duration::from_millis(750);
#[cfg(test)]
const RATE_PROBE_ACK_TIMEOUT: Duration = Duration::from_millis(10);
const RATE_WARMUP_ATTEMPTS: u16 = 2;
const RATE_PROBE_ATTEMPTS: u16 = 3;
static NEXT_RESET_SEQ: AtomicU16 = AtomicU16::new(1);

#[derive(Parser, Debug, Clone)]
struct Args {
    #[arg(long, default_value = "127.0.0.1:25565")]
    listen: String,

    #[arg(long)]
    serial: String,

    #[arg(long, default_value_t = DEFAULT_BAUD)]
    baud: u32,

    #[arg(short, long)]
    verbose: bool,
}

#[derive(Clone, Copy)]
struct BridgeLog {
    verbose: bool,
}

impl BridgeLog {
    fn info(self, message: impl std::fmt::Display) {
        eprintln!("[I] {message}");
    }

    fn debug(self, message: impl std::fmt::Display) {
        if self.verbose {
            eprintln!("[D] {message}");
        }
    }
}

fn hex_preview(data: &[u8]) -> String {
    const MAX_BYTES: usize = 64;
    let shown = data.len().min(MAX_BYTES);
    let mut out = String::new();

    for (i, byte) in data[..shown].iter().enumerate() {
        if i > 0 {
            out.push(' ');
        }
        out.push_str(&format!("{byte:02x}"));
    }
    if data.len() > shown {
        out.push_str(" ...");
    }

    out
}

fn encode_frame(frame_type: link::FrameType, seq: u16, payload: &[u8]) -> io::Result<Vec<u8>> {
    link::encode(frame_type, seq, 0, payload).ok_or_else(|| {
        io::Error::new(
            io::ErrorKind::InvalidInput,
            format!("failed to encode {frame_type:?} frame"),
        )
    })
}

fn write_frame(serial: &mut dyn SerialPort, frame: &[u8]) -> io::Result<()> {
    let mut offset = 0usize;
    let mut backpressure_started = None;

    while offset < frame.len() {
        match serial.write(&frame[offset..]) {
            Ok(0) => {
                return Err(io::Error::new(
                    io::ErrorKind::WriteZero,
                    "failed to write link frame",
                ));
            }
            Ok(n) => {
                offset += n;
                backpressure_started = None;
            }
            Err(ref e)
                if matches!(
                    e.kind(),
                    io::ErrorKind::WouldBlock | io::ErrorKind::TimedOut
                ) =>
            {
                let blocked_since = *backpressure_started.get_or_insert_with(Instant::now);
                if blocked_since.elapsed() >= SERIAL_WRITE_BACKPRESSURE_LIMIT {
                    return Err(io::Error::new(
                        io::ErrorKind::TimedOut,
                        "serial write backpressure limit exceeded",
                    ));
                }
                thread::sleep(Duration::from_millis(1));
            }
            Err(e) => return Err(e),
        }
    }
    serial.flush()
}

fn write_frame_paced(
    serial: &mut dyn SerialPort,
    frame: &[u8],
    chunk_bytes: usize,
    delay: Duration,
    mut sleep: impl FnMut(Duration),
) -> io::Result<()> {
    let chunk_bytes = chunk_bytes.max(1);
    let mut offset = 0usize;

    while offset < frame.len() {
        let end = (offset + chunk_bytes).min(frame.len());
        write_frame(serial, &frame[offset..end])?;
        offset = end;
        if offset < frame.len() && !delay.is_zero() {
            sleep(delay);
        }
    }

    Ok(())
}

fn write_control_frame(serial: &mut dyn SerialPort, frame: &[u8]) -> io::Result<()> {
    write_frame_paced(
        serial,
        frame,
        CONTROL_FRAME_WRITE_CHUNK_BYTES,
        CONTROL_FRAME_WRITE_DELAY,
        thread::sleep,
    )
}

fn write_data_frame_paced(
    serial: &mut dyn SerialPort,
    frame: &[u8],
    profile: rate::RateProfile,
    sleep: impl FnMut(Duration),
) -> io::Result<()> {
    write_frame_paced(serial, frame, 1, profile.inter_byte_sleep, sleep)
}

fn next_reset_seq() -> u16 {
    loop {
        let current = NEXT_RESET_SEQ.load(Ordering::Relaxed);
        let next = if current == u16::MAX {
            1
        } else {
            current + 1
        };
        if NEXT_RESET_SEQ
            .compare_exchange_weak(current, next, Ordering::Relaxed, Ordering::Relaxed)
            .is_ok()
        {
            return current;
        }
    }
}

fn write_link_reset(serial: &mut dyn SerialPort, log: BridgeLog, seq: u16) -> io::Result<()> {
    let frame = encode_frame(link::FrameType::Reset, seq, &[])?;
    log.debug(format_args!("link reset write bytes={}", frame.len()));
    log.debug(format_args!("link reset data {}", hex_preview(&frame)));
    write_control_frame(serial, &frame)
}

fn send_link_reset(serial: &mut dyn SerialPort, log: BridgeLog) -> io::Result<u16> {
    let seq = next_reset_seq();
    write_link_reset(serial, log, seq)?;
    Ok(seq)
}

fn accept_m2c_sequence(
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

struct OutstandingC2m {
    seq: u16,
    payload_len: usize,
    encoded: Vec<u8>,
    sent_at: Instant,
    retries: u8,
}

struct C2mSender {
    pending_tcp: VecDeque<u8>,
    outstanding: Option<OutstandingC2m>,
    next_seq: u16,
    negotiated_payload: usize,
    credit: u16,
}

impl C2mSender {
    fn new(negotiated_payload: usize, credit: u16) -> Self {
        Self {
            pending_tcp: VecDeque::new(),
            outstanding: None,
            next_seq: 0,
            negotiated_payload,
            credit,
        }
    }

    fn push_tcp_bytes(&mut self, data: &[u8]) {
        self.pending_tcp.extend(data);
    }

    fn pending_tcp_len(&self) -> usize {
        self.pending_tcp.len()
    }

    fn reset_link_state_preserving_pending(&mut self, negotiated_payload: usize, credit: u16) {
        self.outstanding = None;
        self.next_seq = 0;
        self.negotiated_payload = negotiated_payload;
        self.credit = credit;
    }

    fn next_frame_to_send(&mut self) -> io::Result<Option<link::Frame>> {
        let Some(payload_len) = self.next_payload_len() else {
            return Ok(None);
        };
        if self.next_seq == u16::MAX {
            return Err(io::Error::new(
                io::ErrorKind::InvalidData,
                "C2M sequence exhausted; reset required",
            ));
        }
        let payload = self.pending_tcp.iter().take(payload_len).copied().collect();

        Ok(Some(link::Frame {
            frame_type: link::FrameType::DataC2m,
            flags: 0,
            seq: self.next_seq,
            ack: 0,
            payload,
        }))
    }

    fn mark_sent(&mut self, frame: &link::Frame) -> io::Result<()> {
        let Some(payload_len) = self.next_payload_len() else {
            return Ok(());
        };
        if frame.seq != self.next_seq || frame.payload.len() != payload_len {
            return Err(io::Error::new(
                io::ErrorKind::InvalidInput,
                "DATA_C2M frame does not match sender state",
            ));
        }
        let encoded = encode_data_c2m_frame(frame)?;

        self.credit = self.credit.saturating_sub(payload_len as u16);
        self.outstanding = Some(OutstandingC2m {
            seq: self.next_seq,
            payload_len,
            encoded,
            sent_at: Instant::now(),
            retries: 0,
        });
        self.next_seq = self.next_seq.wrapping_add(1);
        Ok(())
    }

    fn handle_ack(&mut self, ack: u16, credit: u16) -> io::Result<()> {
        self.credit = credit;

        let Some(outstanding) = self.outstanding.as_ref() else {
            return Ok(());
        };
        if ack != outstanding.seq.wrapping_add(1) {
            return Ok(());
        }

        let payload_len = outstanding.payload_len;
        if self.pending_tcp.len() < payload_len {
            return Err(io::Error::new(
                io::ErrorKind::InvalidData,
                "ACK_C2M advanced beyond pending DATA_C2M bytes",
            ));
        }

        for _ in 0..payload_len {
            self.pending_tcp.pop_front();
        }
        self.outstanding = None;
        Ok(())
    }

    fn has_outstanding(&self) -> bool {
        self.outstanding.is_some()
    }

    fn retransmit_if_due(&mut self, now: Instant, timeout: Duration) -> Option<Vec<u8>> {
        let outstanding = self.outstanding.as_ref()?;
        let Some(elapsed) = now.checked_duration_since(outstanding.sent_at) else {
            return None;
        };
        if elapsed < timeout {
            return None;
        }

        Some(outstanding.encoded.clone())
    }

    fn mark_retransmitted(&mut self, now: Instant) {
        let Some(outstanding) = self.outstanding.as_mut() else {
            return;
        };
        outstanding.sent_at = now;
        outstanding.retries = outstanding.retries.saturating_add(1);
    }

    fn next_payload_len(&self) -> Option<usize> {
        if self.outstanding.is_some() || self.pending_tcp.is_empty() || self.credit == 0 {
            return None;
        }

        let payload_cap = self
            .negotiated_payload
            .max(1)
            .min(link::FIRMWARE_PAYLOAD_CAP);
        let payload_len = self
            .pending_tcp
            .len()
            .min(payload_cap)
            .min(self.credit as usize);
        (payload_len > 0).then_some(payload_len)
    }
}

fn handle_ack_c2m(sender: &mut C2mSender, frame: &link::Frame) -> io::Result<()> {
    let Some(credit) = link::ack_credit(&frame.payload) else {
        return Err(io::Error::new(
            io::ErrorKind::InvalidData,
            "invalid ACK_C2M payload",
        ));
    };
    sender.handle_ack(frame.ack, credit)
}

fn handle_ack_c2m_with_rate(
    sender: &mut C2mSender,
    rate_controller: &mut RateController,
    frame: &link::Frame,
) -> io::Result<()> {
    let had_outstanding = sender.has_outstanding();
    handle_ack_c2m(sender, frame)?;
    if had_outstanding && !sender.has_outstanding() {
        rate_controller.record_success();
    }
    Ok(())
}

fn encode_data_c2m_frame(frame: &link::Frame) -> io::Result<Vec<u8>> {
    link::encode(frame.frame_type, frame.seq, frame.ack, &frame.payload).ok_or_else(|| {
        io::Error::new(
            io::ErrorKind::InvalidInput,
            "failed to encode DATA_C2M frame",
        )
    })
}

fn rate_probe_payload(profile: rate::RateProfile, probe_index: u16) -> [u8; 4] {
    let sleep_us = u16::try_from(profile.inter_byte_sleep.as_micros()).unwrap_or(u16::MAX);
    let sleep_us = sleep_us.to_le_bytes();
    let nonce = probe_index.to_le_bytes();
    [sleep_us[0], sleep_us[1], nonce[0], nonce[1]]
}

fn calibrate_rate(
    serial: &mut dyn SerialPort,
    log: BridgeLog,
    supported_mask: u16,
) -> io::Result<RateController> {
    calibrate_rate_with_sleep(serial, log, supported_mask, thread::sleep)
}

fn calibrate_rate_with_sleep(
    serial: &mut dyn SerialPort,
    log: BridgeLog,
    supported_mask: u16,
    mut sleep: impl FnMut(Duration),
) -> io::Result<RateController> {
    let mut controller = RateController::new();
    let mut decoder = link::Decoder::new();
    let mut pending_frames = VecDeque::new();
    let mut probe_nonce = 0u16;
    let slowest_profile = rate::RATE_PROFILES[0];

    if !RATE_CALIBRATION_SETTLE_DELAY.is_zero() {
        sleep(RATE_CALIBRATION_SETTLE_DELAY);
    }

    let mut warmup_successes = 0u8;
    for _ in 0..RATE_WARMUP_ATTEMPTS {
        if probe_rate_once(
            serial,
            log,
            &mut decoder,
            &mut pending_frames,
            slowest_profile,
            probe_nonce,
            &mut sleep,
        )? {
            warmup_successes = warmup_successes.saturating_add(1);
        }
        probe_nonce = probe_nonce.wrapping_add(1);
    }
    if warmup_successes == 0 {
        return Err(io::Error::new(
            io::ErrorKind::TimedOut,
            format!(
                "rate warmup failed at sleep_us={}",
                slowest_profile.inter_byte_sleep.as_micros()
            ),
        ));
    }
    log.info(format_args!(
        "rate warmup sleep_us={} successes={}/{}",
        slowest_profile.inter_byte_sleep.as_micros(),
        warmup_successes,
        RATE_WARMUP_ATTEMPTS
    ));

    for profile in rate::RATE_PROFILES.iter().copied() {
        if supported_mask & (1u16 << profile.id) == 0 {
            continue;
        }

        let mut successes = 0u8;
        for _ in 0..RATE_PROBE_ATTEMPTS {
            if probe_rate_once(
                serial,
                log,
                &mut decoder,
                &mut pending_frames,
                profile,
                probe_nonce,
                &mut sleep,
            )? {
                successes = successes.saturating_add(1);
            }
            probe_nonce = probe_nonce.wrapping_add(1);
        }

        match successes {
            _ if successes == RATE_PROBE_ATTEMPTS as u8 => {
                controller.mark_profile_stable_with_mask(profile.id, supported_mask);
                log.info(format_args!(
                    "rate sleep_us={} stable successes={}/{}; active_sleep_us={}",
                    profile.inter_byte_sleep.as_micros(),
                    successes,
                    RATE_PROBE_ATTEMPTS,
                    controller.active_profile().inter_byte_sleep.as_micros()
                ));
            }
            2 => {
                controller.mark_profile_stable_with_mask(profile.id, supported_mask);
                log.info(format_args!(
                    "rate sleep_us={} borderline successes={}/{}; active_sleep_us={}",
                    profile.inter_byte_sleep.as_micros(),
                    successes,
                    RATE_PROBE_ATTEMPTS,
                    controller.active_profile().inter_byte_sleep.as_micros()
                ));
                break;
            }
            _ if profile.id == 0 => {
                return Err(io::Error::new(
                    io::ErrorKind::TimedOut,
                    format!(
                        "rate slowest profile failed sleep_us={} successes={}/{}",
                        profile.inter_byte_sleep.as_micros(),
                        successes,
                        RATE_PROBE_ATTEMPTS
                    ),
                ));
            }
            _ => {
                log.info(format_args!(
                    "rate sleep_us={} probe failed successes={}/{}; using sleep_us={}",
                    profile.inter_byte_sleep.as_micros(),
                    successes,
                    RATE_PROBE_ATTEMPTS,
                    controller.active_profile().inter_byte_sleep.as_micros()
                ));
                break;
            }
        }
    }

    Ok(controller)
}

fn probe_rate_once(
    serial: &mut dyn SerialPort,
    log: BridgeLog,
    decoder: &mut link::Decoder,
    pending_frames: &mut VecDeque<link::Frame>,
    profile: rate::RateProfile,
    probe_nonce: u16,
    sleep: &mut impl FnMut(Duration),
) -> io::Result<bool> {
    let payload = rate_probe_payload(profile, probe_nonce);
    let encoded = encode_frame(link::FrameType::RateProbe, probe_nonce, &payload)?;
    log.debug(format_args!(
        "RATE_PROBE sleep_us={} nonce={} bytes={}",
        profile.inter_byte_sleep.as_micros(),
        probe_nonce,
        encoded.len()
    ));
    log.debug(format_args!("RATE_PROBE data {}", hex_preview(&encoded)));
    write_frame_paced(serial, &encoded, 1, profile.inter_byte_sleep, &mut *sleep)?;
    wait_for_rate_probe_ack(
        serial,
        log,
        decoder,
        pending_frames,
        probe_nonce,
        &payload,
        RATE_PROBE_ACK_TIMEOUT,
        sleep,
    )
}

fn wait_for_rate_probe_ack(
    serial: &mut dyn SerialPort,
    log: BridgeLog,
    decoder: &mut link::Decoder,
    pending_frames: &mut VecDeque<link::Frame>,
    expected_seq: u16,
    expected_payload: &[u8],
    timeout: Duration,
    sleep: &mut impl FnMut(Duration),
) -> io::Result<bool> {
    let start = Instant::now();
    let mut buf = [0u8; IO_BUF_LEN];

    while start.elapsed() < timeout {
        while let Some(frame) = pending_frames.pop_front() {
            if rate_probe_ack_matches(&frame, expected_seq, expected_payload, log)? {
                return Ok(true);
            }
        }

        match serial.read(&mut buf) {
            Ok(0) => sleep(Duration::from_millis(1)),
            Ok(n) => {
                log.debug(format_args!(
                    "rate calibration read bytes={} data {}",
                    n,
                    hex_preview(&buf[..n])
                ));
                let mut matched = false;
                for event in decoder.feed(&buf[..n]) {
                    let frame = match event {
                        link::DecodeEvent::Frame(frame) => frame,
                        link::DecodeEvent::Error(error) => {
                            log.info(format_args!(
                                "link decode error {error:?} during rate calibration"
                            ));
                            continue;
                        }
                    };
                    if matched {
                        if matches!(frame.frame_type, link::FrameType::DataM2c) {
                            log.info("DATA_M2C received during rate calibration");
                            return Err(io::Error::new(
                                io::ErrorKind::InvalidData,
                                "DATA_M2C received during rate calibration",
                            ));
                        }
                        pending_frames.push_back(frame);
                        continue;
                    }
                    matched = rate_probe_ack_matches(&frame, expected_seq, expected_payload, log)?;
                }
                if matched {
                    return Ok(true);
                }
            }
            Err(ref e)
                if matches!(
                    e.kind(),
                    io::ErrorKind::WouldBlock | io::ErrorKind::TimedOut
                ) =>
            {
                sleep(Duration::from_millis(1));
            }
            Err(e) => return Err(e),
        }
    }

    Ok(false)
}

fn rate_probe_ack_matches(
    frame: &link::Frame,
    expected_seq: u16,
    expected_payload: &[u8],
    log: BridgeLog,
) -> io::Result<bool> {
    match frame.frame_type {
        link::FrameType::RateProbeAck => {
            if frame.seq == expected_seq && frame.payload == expected_payload {
                Ok(true)
            } else {
                log.debug(format_args!(
                    "ignored RATE_PROBE_ACK seq={} expected_seq={} payload={}",
                    frame.seq,
                    expected_seq,
                    hex_preview(&frame.payload)
                ));
                Ok(false)
            }
        }
        link::FrameType::DataM2c => {
            log.info("DATA_M2C received during rate calibration");
            Err(io::Error::new(
                io::ErrorKind::InvalidData,
                "DATA_M2C received during rate calibration",
            ))
        }
        link::FrameType::Error => {
            log.info(format_args!(
                "link ERROR during rate calibration payload={}",
                hex_preview(&frame.payload)
            ));
            Ok(false)
        }
        link::FrameType::Unknown(raw) => {
            log.info(format_args!(
                "link unknown frame type=0x{raw:02x} during rate calibration"
            ));
            Ok(false)
        }
        other => {
            log.debug(format_args!(
                "link ignored {other:?} during rate calibration"
            ));
            Ok(false)
        }
    }
}

fn reset_c2m_sender(
    negotiated_payload: usize,
    credit: u16,
    sender: &mut C2mSender,
    m2c_seq_expected: &mut u16,
) {
    *sender = C2mSender::new(negotiated_payload, credit);
    *m2c_seq_expected = 0;
}

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
struct ReadyState {
    negotiated_payload: usize,
    initial_credit: u16,
    supported_rate_mask: u16,
}

fn reset_link_session_state(
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

fn reset_link_session_state_preserving_pending(
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

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
enum RetransmitTimeoutAction {
    None,
    Retransmitted,
    Reset,
}

fn handle_retransmit_timeout(
    c2m_sender: &mut C2mSender,
    serial: &mut dyn SerialPort,
    log: BridgeLog,
    negotiated_payload: &mut usize,
    credit: &mut u16,
    supported_rate_mask: &mut u16,
    rate_controller: &mut RateController,
    m2c_seq_expected: &mut u16,
    sleep: impl FnMut(Duration),
) -> io::Result<RetransmitTimeoutAction> {
    let Some(encoded) = c2m_sender.retransmit_if_due(Instant::now(), C2M_RETRANSMIT_TIMEOUT) else {
        return Ok(RetransmitTimeoutAction::None);
    };
    rate_controller.record_loss();
    if rate_controller.needs_recalibration() {
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

fn configure_tcp_stream(tcp: &TcpStream) -> io::Result<()> {
    tcp.set_read_timeout(Some(TCP_READ_TIMEOUT))?;
    tcp.set_write_timeout(Some(TCP_WRITE_TIMEOUT))
}

fn normalize_ready(ready: link::ReadyPayload) -> Option<ReadyState> {
    if ready.negotiated_payload == 0 || ready.credit_cap == 0 || ready.initial_credit == 0 {
        return None;
    }
    if ready.initial_credit > ready.credit_cap {
        return None;
    }
    if ready.initial_rate_profile != link::STARTUP_RATE_PROFILE {
        return None;
    }
    if ready.supported_rate_mask & (1u16 << ready.initial_rate_profile) == 0 {
        return None;
    }
    Some(ReadyState {
        negotiated_payload: usize::from(ready.negotiated_payload).min(link::FIRMWARE_PAYLOAD_CAP),
        initial_credit: ready.initial_credit,
        supported_rate_mask: ready.supported_rate_mask,
    })
}

fn wait_for_ready(serial: &mut dyn SerialPort, log: BridgeLog) -> io::Result<ReadyState> {
    let hello_payload = link::hello_payload(link::DEFAULT_PAYLOAD as u16, link::INITIAL_CREDIT);
    let hello = encode_frame(link::FrameType::Hello, 0, &hello_payload)?;
    let start = Instant::now();
    let mut last_hello = None;
    let mut decoder = link::Decoder::new();
    let mut buf = [0u8; IO_BUF_LEN];

    while start.elapsed() < LINK_READY_TIMEOUT {
        if last_hello
            .map(|sent: Instant| sent.elapsed() >= LINK_HELLO_INTERVAL)
            .unwrap_or(true)
        {
            log.debug(format_args!("link hello write bytes={}", hello.len()));
            log.debug(format_args!("link hello data {}", hex_preview(&hello)));
            write_control_frame(serial, &hello)?;
            last_hello = Some(Instant::now());
        }

        match serial.read(&mut buf) {
            Ok(0) => thread::sleep(Duration::from_millis(1)),
            Ok(n) => {
                log.debug(format_args!("link wait read bytes={n}"));
                for event in decoder.feed(&buf[..n]) {
                    let frame = match event {
                        link::DecodeEvent::Frame(frame) => frame,
                        link::DecodeEvent::Error(error) => {
                            log.info(format_args!(
                                "link decode error {error:?} while waiting for ready"
                            ));
                            continue;
                        }
                    };
                    match frame.frame_type {
                        link::FrameType::Ready => {
                            let Some(ready) = link::parse_ready(&frame.payload) else {
                                log.info(format_args!(
                                    "link READY with invalid payload {}",
                                    hex_preview(&frame.payload)
                                ));
                                continue;
                            };
                            let Some(ready_state) = normalize_ready(ready) else {
                                log.info(format_args!(
                                    "link READY with invalid values payload={} credit_cap={} credit={} rate_mask=0x{:04x} rate_profile={} flags=0x{:04x}",
                                    ready.negotiated_payload,
                                    ready.credit_cap,
                                    ready.initial_credit,
                                    ready.supported_rate_mask,
                                    ready.initial_rate_profile,
                                    ready.flags
                                ));
                                continue;
                            };
                            log.info(format_args!(
                                "link ready payload={} credit={} rate_mask=0x{:04x} flags=0x{:04x}",
                                ready_state.negotiated_payload,
                                ready_state.initial_credit,
                                ready.supported_rate_mask,
                                ready.flags
                            ));
                            return Ok(ready_state);
                        }
                        link::FrameType::ResetAck => {
                            log.debug("link reset ack while waiting for ready");
                        }
                        link::FrameType::Error => {
                            log.info(format_args!(
                                "link error while waiting for ready payload={}",
                                hex_preview(&frame.payload)
                            ));
                        }
                        link::FrameType::Unknown(raw) => {
                            log.info(format_args!(
                                "link unknown frame type=0x{raw:02x} while waiting for ready"
                            ));
                        }
                        other => {
                            log.debug(format_args!(
                                "link ignored {other:?} while waiting for ready"
                            ));
                        }
                    }
                }
            }
            Err(ref e)
                if matches!(
                    e.kind(),
                    io::ErrorKind::WouldBlock | io::ErrorKind::TimedOut
                ) =>
            {
                thread::sleep(Duration::from_millis(1));
            }
            Err(e) => return Err(e),
        }
    }

    Err(io::Error::new(
        io::ErrorKind::TimedOut,
        "timed out waiting for link READY",
    ))
}

fn reset_link_and_wait_ready(serial: &mut dyn SerialPort, log: BridgeLog) -> io::Result<ReadyState> {
    reset_link_and_wait_ready_with_seq(serial, log, next_reset_seq(), thread::sleep)
}

fn reset_link_and_wait_ready_with_seq(
    serial: &mut dyn SerialPort,
    log: BridgeLog,
    reset_seq: u16,
    mut sleep: impl FnMut(Duration),
) -> io::Result<ReadyState> {
    let start = Instant::now();
    let mut reset_sent = false;
    let mut saw_reset_ack = false;
    let mut decoder = link::Decoder::new();
    let mut buf = [0u8; IO_BUF_LEN];

    while start.elapsed() < LINK_READY_TIMEOUT {
        if !reset_sent {
            write_link_reset(serial, log, reset_seq)?;
            reset_sent = true;
        }

        match serial.read(&mut buf) {
            Ok(0) => sleep(Duration::from_millis(1)),
            Ok(n) => {
                log.debug(format_args!("link reset wait read bytes={n}"));
                for event in decoder.feed(&buf[..n]) {
                    let frame = match event {
                        link::DecodeEvent::Frame(frame) => frame,
                        link::DecodeEvent::Error(error) => {
                            log.info(format_args!(
                                "link decode error {error:?} while waiting for reset ready"
                            ));
                            continue;
                        }
                    };
                    match frame.frame_type {
                        link::FrameType::ResetAck if frame.seq == reset_seq => {
                            saw_reset_ack = true;
                            log.debug(format_args!("link reset ack seq={}", frame.seq));
                        }
                        link::FrameType::ResetAck => {
                            log.debug(format_args!(
                                "ignored stale reset ack seq={} expected={reset_seq}",
                                frame.seq
                            ));
                        }
                        link::FrameType::Ready if saw_reset_ack => {
                            let Some(ready) = link::parse_ready(&frame.payload) else {
                                log.info(format_args!(
                                    "link READY with invalid payload {}",
                                    hex_preview(&frame.payload)
                                ));
                                continue;
                            };
                            let Some(ready_state) = normalize_ready(ready) else {
                                log.info(format_args!(
                                    "link READY with invalid values payload={} credit_cap={} credit={} rate_mask=0x{:04x} rate_profile={} flags=0x{:04x}",
                                    ready.negotiated_payload,
                                    ready.credit_cap,
                                    ready.initial_credit,
                                    ready.supported_rate_mask,
                                    ready.initial_rate_profile,
                                    ready.flags
                                ));
                                continue;
                            };
                            log.info(format_args!(
                                "link reset ready payload={} credit={} rate_mask=0x{:04x} flags=0x{:04x}",
                                ready_state.negotiated_payload,
                                ready_state.initial_credit,
                                ready_state.supported_rate_mask,
                                ready.flags
                            ));
                            return Ok(ready_state);
                        }
                        link::FrameType::Ready => {
                            log.debug("ignored READY before matching reset ack");
                        }
                        link::FrameType::Error => {
                            log.info(format_args!(
                                "link error while waiting for reset ready payload={}",
                                hex_preview(&frame.payload)
                            ));
                        }
                        link::FrameType::Unknown(raw) => {
                            log.info(format_args!(
                                "link unknown frame type=0x{raw:02x} while waiting for reset ready"
                            ));
                        }
                        other => {
                            log.debug(format_args!(
                                "link ignored {other:?} while waiting for reset ready"
                            ));
                        }
                    }
                }
            }
            Err(ref e)
                if matches!(
                    e.kind(),
                    io::ErrorKind::WouldBlock | io::ErrorKind::TimedOut
                ) =>
            {
                sleep(Duration::from_millis(1));
            }
            Err(e) => return Err(e),
        }
    }

    Err(io::Error::new(
        io::ErrorKind::TimedOut,
        "timed out waiting for reset READY",
    ))
}

fn run_link_client(
    mut tcp: TcpStream,
    serial: &mut dyn SerialPort,
    log: BridgeLog,
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
    let mut serial_buf = [0u8; IO_BUF_LEN];
    let mut tcp_read_total = 0usize;
    let mut tcp_to_serial_total = 0usize;
    let mut serial_to_tcp_total = 0usize;

    loop {
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
                    c2m_sender.push_tcp_bytes(&tcp_buf[..n]);
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
            &mut negotiated_payload,
            &mut credit,
            &mut supported_rate_mask,
            &mut rate_controller,
            &mut m2c_seq_expected,
            thread::sleep,
        ) {
            Ok(RetransmitTimeoutAction::Reset) => {
                decoder = link::Decoder::new();
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
                    reset_link_session_state_preserving_pending(
                        serial,
                        log,
                        &mut negotiated_payload,
                        &mut credit,
                        &mut supported_rate_mask,
                        &mut rate_controller,
                        &mut c2m_sender,
                        &mut m2c_seq_expected,
                    )?;
                    decoder = link::Decoder::new();
                    thread::sleep(Duration::from_millis(1));
                    continue;
                }
                Err(e) => return Err(e),
            },
        }

        match serial.read(&mut serial_buf) {
            Ok(0) => {}
            Ok(n) => {
                log.debug(format_args!("serial link read bytes={n}"));
                for event in decoder.feed(&serial_buf[..n]) {
                    let frame = match event {
                        link::DecodeEvent::Frame(frame) => frame,
                        link::DecodeEvent::Error(error) => {
                            return Err(io::Error::new(
                                io::ErrorKind::InvalidData,
                                format!("serial link decode error: {error:?}"),
                            ));
                        }
                    };
                    match frame.frame_type {
                        link::FrameType::DataM2c => {
                            accept_m2c_sequence(&frame, &mut m2c_seq_expected, log)?;
                            if let Err(e) = tcp.write_all(&frame.payload) {
                                let _ = send_link_reset(serial, log);
                                return Err(e);
                            }
                            serial_to_tcp_total += frame.payload.len();
                            log.debug(format_args!(
                                "DATA_M2C wrote tcp bytes={} total={serial_to_tcp_total}",
                                frame.payload.len()
                            ));
                        }
                        link::FrameType::AckC2m => {
                            handle_ack_c2m_with_rate(
                                &mut c2m_sender,
                                &mut rate_controller,
                                &frame,
                            )?;
                            credit = c2m_sender.credit;
                            log.debug(format_args!(
                                "ACK_C2M ack={} credit={credit} pending={}",
                                frame.ack,
                                c2m_sender.pending_tcp_len()
                            ));
                        }
                        link::FrameType::Ready => {
                            if let Some(ready) = link::parse_ready(&frame.payload) {
                                if normalize_ready(ready).is_some() {
                                    reset_link_session_state(
                                        serial,
                                        log,
                                        &mut negotiated_payload,
                                        &mut credit,
                                        &mut supported_rate_mask,
                                        &mut rate_controller,
                                        &mut c2m_sender,
                                        &mut m2c_seq_expected,
                                    )?;
                                    decoder = link::Decoder::new();
                                    log.info(format_args!(
                                        "link ready update payload={negotiated_payload} credit={credit} rate_mask=0x{:04x} flags=0x{:04x}",
                                        supported_rate_mask,
                                        ready.flags
                                    ));
                                    break;
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
                            reset_link_session_state(
                                serial,
                                log,
                                &mut negotiated_payload,
                                &mut credit,
                                &mut supported_rate_mask,
                                &mut rate_controller,
                                &mut c2m_sender,
                                &mut m2c_seq_expected,
                            )?;
                            decoder = link::Decoder::new();
                            break;
                        }
                        link::FrameType::Ping => {
                            let pong =
                                encode_frame(link::FrameType::Pong, frame.seq, &frame.payload)?;
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
                }
            }
            Err(ref e)
                if matches!(
                    e.kind(),
                    io::ErrorKind::WouldBlock | io::ErrorKind::TimedOut
                ) => {}
            Err(e) => return Err(e),
        }

        thread::sleep(Duration::from_millis(1));
    }
}

fn main() -> io::Result<()> {
    let args = Args::parse();
    let log = BridgeLog {
        verbose: args.verbose,
    };
    let listener = TcpListener::bind(&args.listen)?;
    log.info(format_args!("listening on {}", args.listen));
    log.info(format_args!("serial {} at {}", args.serial, args.baud));
    let mut serial = serialport::new(&args.serial, args.baud)
        .timeout(Duration::from_millis(1))
        .open()?;
    log.debug("serial opened");
    let _ = wait_for_ready(&mut *serial, log)?;

    for stream in listener.incoming() {
        let stream = stream?;
        stream.set_nodelay(true)?;
        let peer = stream.peer_addr().ok();
        log.info(format_args!("client connected peer={peer:?}"));

        match run_link_client(stream, &mut *serial, log) {
            Ok(()) => log.info("client disconnected"),
            Err(e) if e.kind() == io::ErrorKind::InvalidData => return Err(e),
            Err(e) => log.info(format_args!("client link session error: {e}")),
        }
    }

    Ok(())
}

#[cfg(test)]
mod tests {
    use super::*;
    use serialport::{ClearBuffer, DataBits, FlowControl, Parity, StopBits};
    use std::collections::VecDeque;

    enum WriteAction {
        Write(usize),
        Error(io::ErrorKind),
    }

    struct FakeSerialPort {
        actions: VecDeque<WriteAction>,
        readable: VecDeque<u8>,
        written: Vec<u8>,
        max_read_len: Option<usize>,
        read_chunk_lens: VecDeque<usize>,
        readable_after_written_bytes: VecDeque<(usize, Vec<u8>)>,
        auto_reset_ready_payload: Option<Vec<u8>>,
        read_delay: Option<Duration>,
        empty_read_error: io::ErrorKind,
    }

    impl FakeSerialPort {
        fn new(actions: impl IntoIterator<Item = WriteAction>) -> Self {
            Self {
                actions: actions.into_iter().collect(),
                readable: VecDeque::new(),
                written: Vec::new(),
                max_read_len: None,
                read_chunk_lens: VecDeque::new(),
                readable_after_written_bytes: VecDeque::new(),
                auto_reset_ready_payload: None,
                read_delay: None,
                empty_read_error: io::ErrorKind::TimedOut,
            }
        }

        fn with_readable(mut self, readable: impl IntoIterator<Item = u8>) -> Self {
            self.readable = readable.into_iter().collect();
            self
        }

        fn with_readable_after_written_bytes(
            mut self,
            written_len: usize,
            readable: impl IntoIterator<Item = u8>,
        ) -> Self {
            self.readable_after_written_bytes
                .push_back((written_len, readable.into_iter().collect()));
            self
        }

        fn with_max_read_len(mut self, max_read_len: usize) -> Self {
            self.max_read_len = Some(max_read_len);
            self
        }

        fn with_auto_reset_ready(mut self, ready_payload: &[u8]) -> Self {
            self.auto_reset_ready_payload = Some(ready_payload.to_vec());
            self
        }

        fn with_read_delay(mut self, delay: Duration) -> Self {
            self.read_delay = Some(delay);
            self
        }

        fn with_empty_read_error(mut self, kind: io::ErrorKind) -> Self {
            self.empty_read_error = kind;
            self
        }

        fn maybe_queue_scheduled_readable(&mut self) {
            while let Some((written_len, _)) = self.readable_after_written_bytes.front() {
                if self.written.len() < *written_len {
                    break;
                }
                let (_, readable) = self.readable_after_written_bytes.pop_front().unwrap();
                self.readable.extend(readable);
            }
        }

        fn maybe_queue_auto_reset_ready(&mut self) {
            let Some(ready_payload) = self.auto_reset_ready_payload.take() else {
                return;
            };
            let mut decoder = link::Decoder::new();
            let mut reset_seq = None;
            for event in decoder.feed(&self.written) {
                if let link::DecodeEvent::Frame(frame) = event {
                    if matches!(frame.frame_type, link::FrameType::Reset) {
                        reset_seq = Some(frame.seq);
                    }
                }
            }
            let Some(reset_seq) = reset_seq else {
                self.auto_reset_ready_payload = Some(ready_payload);
                return;
            };

            let reset_ack = link::encode(link::FrameType::ResetAck, reset_seq, 0, &[]).unwrap();
            let ready = link::encode(link::FrameType::Ready, 0, 0, &ready_payload).unwrap();
            let mut readable = reset_ack.clone();
            readable.extend_from_slice(&ready);
            readable.extend(self.readable.drain(..));
            self.readable = readable.into_iter().collect();

            let mut chunk_lens = VecDeque::new();
            chunk_lens.push_back(reset_ack.len());
            chunk_lens.push_back(ready.len());
            chunk_lens.extend(self.read_chunk_lens.drain(..));
            self.read_chunk_lens = chunk_lens;
        }
    }

    impl Read for FakeSerialPort {
        fn read(&mut self, buf: &mut [u8]) -> io::Result<usize> {
            self.maybe_queue_auto_reset_ready();
            self.maybe_queue_scheduled_readable();
            if self.readable.is_empty() {
                return Err(io::Error::new(self.empty_read_error, "not readable"));
            }
            if let Some(delay) = self.read_delay {
                thread::sleep(delay);
            }

            let n = buf
                .len()
                .min(self.readable.len())
                .min(self.read_chunk_lens.pop_front().unwrap_or(usize::MAX))
                .min(self.max_read_len.unwrap_or(usize::MAX));
            for slot in &mut buf[..n] {
                *slot = self.readable.pop_front().unwrap();
            }
            Ok(n)
        }
    }

    impl Write for FakeSerialPort {
        fn write(&mut self, buf: &[u8]) -> io::Result<usize> {
            match self
                .actions
                .pop_front()
                .unwrap_or(WriteAction::Write(buf.len()))
            {
                WriteAction::Write(n) => {
                    let n = n.min(buf.len());
                    self.written.extend_from_slice(&buf[..n]);
                    self.maybe_queue_scheduled_readable();
                    Ok(n)
                }
                WriteAction::Error(kind) => Err(io::Error::new(kind, "write blocked")),
            }
        }

        fn flush(&mut self) -> io::Result<()> {
            Ok(())
        }
    }

    impl SerialPort for FakeSerialPort {
        fn name(&self) -> Option<String> {
            Some("fake".to_string())
        }

        fn baud_rate(&self) -> serialport::Result<u32> {
            Ok(921600)
        }

        fn data_bits(&self) -> serialport::Result<DataBits> {
            Ok(DataBits::Eight)
        }

        fn flow_control(&self) -> serialport::Result<FlowControl> {
            Ok(FlowControl::None)
        }

        fn parity(&self) -> serialport::Result<Parity> {
            Ok(Parity::None)
        }

        fn stop_bits(&self) -> serialport::Result<StopBits> {
            Ok(StopBits::One)
        }

        fn timeout(&self) -> Duration {
            Duration::from_millis(1)
        }

        fn set_baud_rate(&mut self, _baud_rate: u32) -> serialport::Result<()> {
            Ok(())
        }

        fn set_data_bits(&mut self, _data_bits: DataBits) -> serialport::Result<()> {
            Ok(())
        }

        fn set_flow_control(&mut self, _flow_control: FlowControl) -> serialport::Result<()> {
            Ok(())
        }

        fn set_parity(&mut self, _parity: Parity) -> serialport::Result<()> {
            Ok(())
        }

        fn set_stop_bits(&mut self, _stop_bits: StopBits) -> serialport::Result<()> {
            Ok(())
        }

        fn set_timeout(&mut self, _timeout: Duration) -> serialport::Result<()> {
            Ok(())
        }

        fn write_request_to_send(&mut self, _level: bool) -> serialport::Result<()> {
            Ok(())
        }

        fn write_data_terminal_ready(&mut self, _level: bool) -> serialport::Result<()> {
            Ok(())
        }

        fn read_clear_to_send(&mut self) -> serialport::Result<bool> {
            Ok(true)
        }

        fn read_data_set_ready(&mut self) -> serialport::Result<bool> {
            Ok(true)
        }

        fn read_ring_indicator(&mut self) -> serialport::Result<bool> {
            Ok(false)
        }

        fn read_carrier_detect(&mut self) -> serialport::Result<bool> {
            Ok(true)
        }

        fn bytes_to_read(&self) -> serialport::Result<u32> {
            Ok(0)
        }

        fn bytes_to_write(&self) -> serialport::Result<u32> {
            Ok(0)
        }

        fn clear(&self, _buffer_to_clear: ClearBuffer) -> serialport::Result<()> {
            Ok(())
        }

        fn try_clone(&self) -> serialport::Result<Box<dyn SerialPort>> {
            Ok(Box::new(Self::new([])))
        }

        fn set_break(&self) -> serialport::Result<()> {
            Ok(())
        }

        fn clear_break(&self) -> serialport::Result<()> {
            Ok(())
        }
    }

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

        let err = run_link_client(server, &mut serial, log).unwrap_err();

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

        let err = run_link_client(server, &mut serial, log).unwrap_err();

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
    fn retransmit_recalibration_uses_reset_preserving_pending_and_skips_stale_write() {
        let mut sender = C2mSender::new(497, 512);
        sender.push_tcp_bytes(b"abc");
        let frame = sender.next_frame_to_send().unwrap().unwrap();
        sender.mark_sent(&frame).unwrap();
        sender.outstanding.as_mut().unwrap().sent_at =
            Instant::now() - C2M_RETRANSMIT_TIMEOUT - Duration::from_millis(1);
        let mut controller = RateController::new();
        controller.mark_profile_stable(3);
        for _ in 0..6 {
            controller.record_loss();
        }
        let mut negotiated_payload = 497usize;
        let mut credit = 512u16;
        let mut supported_rate_mask = 0x0001u16;
        let mut m2c_seq_expected = 7u16;
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
                &mut negotiated_payload,
                &mut credit,
                &mut supported_rate_mask,
                &mut controller,
                &mut m2c_seq_expected,
                |_| {}
            )
            .unwrap(),
            RetransmitTimeoutAction::Reset
        );

        assert_eq!(sender.pending_tcp_len(), 3);
        assert!(sender.outstanding.is_none());
        assert_eq!(sender.next_seq, 0);
        assert_eq!(m2c_seq_expected, 0);
        let mut decoder = link::Decoder::new();
        let frames: Vec<_> = decoder
            .feed(&serial.written)
            .into_iter()
            .map(|event| match event {
                link::DecodeEvent::Frame(frame) => frame,
                link::DecodeEvent::Error(error) => panic!("decode error: {error:?}"),
            })
            .collect();
        assert_eq!(frames[0].frame_type, link::FrameType::Reset);
        assert!(frames
            .iter()
            .all(|frame| frame.frame_type != link::FrameType::DataC2m));
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
}
