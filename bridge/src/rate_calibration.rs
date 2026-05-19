use crate::config::{
    IO_BUF_LEN, RATE_CALIBRATION_SETTLE_DELAY, RATE_PROBE_ACK_TIMEOUT, RATE_PROBE_ATTEMPTS,
    RATE_WARMUP_ATTEMPTS,
};
use crate::link;
use crate::log::{hex_preview, BridgeLog};
use crate::rate::{self, RateController};
use crate::serial_io::{encode_frame, write_frame_paced};
use serialport::SerialPort;
use std::collections::VecDeque;
use std::io;
use std::thread;
use std::time::{Duration, Instant};

pub(crate) fn rate_probe_payload(profile: rate::RateProfile, probe_index: u16) -> [u8; 4] {
    let sleep_us = u16::try_from(profile.inter_byte_sleep.as_micros()).unwrap_or(u16::MAX);
    let sleep_us = sleep_us.to_le_bytes();
    let nonce = probe_index.to_le_bytes();
    [sleep_us[0], sleep_us[1], nonce[0], nonce[1]]
}

pub(crate) fn calibrate_rate(
    serial: &mut dyn SerialPort,
    log: BridgeLog,
    supported_mask: u16,
) -> io::Result<RateController> {
    calibrate_rate_with_sleep(serial, log, supported_mask, thread::sleep)
}

pub(crate) fn calibrate_rate_with_sleep(
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

pub(crate) fn probe_rate_once(
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

pub(crate) fn wait_for_rate_probe_ack(
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

pub(crate) fn rate_probe_ack_matches(
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
