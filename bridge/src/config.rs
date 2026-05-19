use std::time::Duration;

pub(crate) const DEFAULT_BAUD: u32 = 115200;
pub(crate) const TCP_PENDING_LIMIT: usize = 8192;
pub(crate) const IO_BUF_LEN: usize = 8192;
pub(crate) const SERIAL_RX_DRAIN_MAX_EVENTS: usize = 32;
pub(crate) const SERIAL_RX_DRAIN_MAX_BYTES: usize = IO_BUF_LEN * 4;
pub(crate) const TCP_READ_TIMEOUT: Duration = Duration::from_millis(1);
pub(crate) const TCP_WRITE_TIMEOUT: Duration = Duration::from_secs(1);
pub(crate) const SERIAL_WRITE_BACKPRESSURE_LIMIT: Duration = Duration::from_secs(1);
pub(crate) const LINK_READY_TIMEOUT: Duration = Duration::from_secs(5);
pub(crate) const LINK_HELLO_INTERVAL: Duration = Duration::from_millis(100);
pub(crate) const CONTROL_FRAME_WRITE_CHUNK_BYTES: usize = 1;
pub(crate) const CONTROL_FRAME_WRITE_DELAY: Duration = Duration::from_millis(10);
pub(crate) const DEFAULT_C2M_RETRANSMIT_TIMEOUT_MS: u64 = 1000;
#[cfg(not(test))]
pub(crate) const RATE_CALIBRATION_SETTLE_DELAY: Duration = Duration::from_millis(50);
#[cfg(test)]
pub(crate) const RATE_CALIBRATION_SETTLE_DELAY: Duration = Duration::ZERO;
#[cfg(not(test))]
pub(crate) const RATE_PROBE_ACK_TIMEOUT: Duration = Duration::from_millis(750);
#[cfg(test)]
pub(crate) const RATE_PROBE_ACK_TIMEOUT: Duration = Duration::from_millis(10);
pub(crate) const RATE_WARMUP_ATTEMPTS: u16 = 2;
pub(crate) const RATE_PROBE_ATTEMPTS: u16 = 3;
