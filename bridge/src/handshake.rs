use crate::config::{IO_BUF_LEN, LINK_HELLO_INTERVAL, LINK_READY_TIMEOUT};
use crate::link;
use crate::log::{hex_preview, BridgeLog};
use crate::serial_io::{encode_frame, next_reset_seq, write_control_frame, write_link_reset};
use serialport::SerialPort;
use std::io::{self, Read};
use std::thread;
use std::time::{Duration, Instant};

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub(crate) struct ReadyState {
    pub(crate) negotiated_payload: usize,
    pub(crate) initial_credit: u16,
    pub(crate) supported_rate_mask: u16,
}

pub(crate) fn normalize_ready(ready: link::ReadyPayload) -> Option<ReadyState> {
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

pub(crate) fn wait_for_ready(serial: &mut dyn SerialPort, log: BridgeLog) -> io::Result<ReadyState> {
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

pub(crate) fn reset_link_and_wait_ready(serial: &mut dyn SerialPort, log: BridgeLog) -> io::Result<ReadyState> {
    reset_link_and_wait_ready_with_seq(serial, log, next_reset_seq(), thread::sleep)
}

pub(crate) fn reset_link_and_wait_ready_with_seq(
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
