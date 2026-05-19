use crate::config::{
    CONTROL_FRAME_WRITE_CHUNK_BYTES, CONTROL_FRAME_WRITE_DELAY, SERIAL_WRITE_BACKPRESSURE_LIMIT,
};
use crate::link;
use crate::log::{hex_preview, BridgeLog};
use crate::rate;
use serialport::SerialPort;
use std::io::{self, Write};
use std::sync::atomic::{AtomicU16, Ordering};
use std::thread;
use std::time::{Duration, Instant};

static NEXT_RESET_SEQ: AtomicU16 = AtomicU16::new(1);

pub(crate) fn encode_frame(frame_type: link::FrameType, seq: u16, payload: &[u8]) -> io::Result<Vec<u8>> {
    link::encode(frame_type, seq, 0, payload).ok_or_else(|| {
        io::Error::new(
            io::ErrorKind::InvalidInput,
            format!("failed to encode {frame_type:?} frame"),
        )
    })
}

pub(crate) fn write_frame(serial: &mut dyn SerialPort, frame: &[u8]) -> io::Result<()> {
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

pub(crate) fn write_frame_paced(
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

pub(crate) fn write_control_frame(serial: &mut dyn SerialPort, frame: &[u8]) -> io::Result<()> {
    write_frame_paced(
        serial,
        frame,
        CONTROL_FRAME_WRITE_CHUNK_BYTES,
        CONTROL_FRAME_WRITE_DELAY,
        thread::sleep,
    )
}

pub(crate) fn write_data_frame_paced(
    serial: &mut dyn SerialPort,
    frame: &[u8],
    profile: rate::RateProfile,
    sleep: impl FnMut(Duration),
) -> io::Result<()> {
    write_frame_paced(serial, frame, 1, profile.inter_byte_sleep, sleep)
}

pub(crate) fn next_reset_seq() -> u16 {
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

pub(crate) fn write_link_reset(serial: &mut dyn SerialPort, log: BridgeLog, seq: u16) -> io::Result<()> {
    let frame = encode_frame(link::FrameType::Reset, seq, &[])?;
    log.debug(format_args!("link reset write bytes={}", frame.len()));
    log.debug(format_args!("link reset data {}", hex_preview(&frame)));
    write_control_frame(serial, &frame)
}

pub(crate) fn send_link_reset(serial: &mut dyn SerialPort, log: BridgeLog) -> io::Result<u16> {
    let seq = next_reset_seq();
    write_link_reset(serial, log, seq)?;
    Ok(seq)
}
