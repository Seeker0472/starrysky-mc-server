use clap::Parser;
use serialport::SerialPort;
use std::io::{self, Read, Write};
use std::net::{Shutdown, TcpListener, TcpStream};
use std::sync::{
    atomic::{AtomicBool, Ordering},
    Arc,
};
use std::thread;
use std::time::{Duration, Instant};

const SERIAL_WRITE_BACKPRESSURE_LIMIT: Duration = Duration::from_secs(1);
const BRIDGE_RESET_FRAME: [u8; 10] = [0xff, 0x00, 0xff, b'M', b'C', b'U', b'R', b'S', b'T', 0x7e];
const BRIDGE_RESET_SETTLE: Duration = Duration::from_millis(20);
const DEFAULT_BAUD: u32 = 115200;
const DEFAULT_SERIAL_WRITE_CHUNK_BYTES: usize = 1;
const DEFAULT_SERIAL_WRITE_DELAY: Duration = Duration::from_millis(10);

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

fn send_bridge_reset(serial: &mut dyn SerialPort, log: BridgeLog) -> io::Result<()> {
    log.debug(format_args!("serial reset write bytes={}", BRIDGE_RESET_FRAME.len()));
    log.debug(format_args!("serial reset data {}", hex_preview(&BRIDGE_RESET_FRAME)));
    write_serial_all_with_retry_and_pacing(
        serial,
        &BRIDGE_RESET_FRAME,
        &AtomicBool::new(false),
        || Ok(false),
        DEFAULT_SERIAL_WRITE_CHUNK_BYTES,
        DEFAULT_SERIAL_WRITE_DELAY,
        thread::sleep,
    )?;
    serial.flush()?;
    thread::sleep(BRIDGE_RESET_SETTLE);
    let _ = serial.clear(serialport::ClearBuffer::Input);
    log.debug("serial input cleared after reset");
    Ok(())
}

fn send_bridge_reset_from_handle(serial: &dyn SerialPort, log: BridgeLog) -> io::Result<()> {
    let mut reset_serial = serial.try_clone()?;
    send_bridge_reset(&mut *reset_serial, log)
}

fn write_serial_all_with_retry(
    serial: &mut dyn SerialPort,
    data: &[u8],
    stop: &AtomicBool,
    mut tcp_peer_disconnected: impl FnMut() -> io::Result<bool>,
) -> io::Result<()> {
    let mut offset = 0;
    let mut backpressure_started = None;

    while offset < data.len() {
        if stop.load(Ordering::Relaxed) {
            return Ok(());
        }

        match serial.write(&data[offset..]) {
            Ok(0) => {
                return Err(io::Error::new(
                    io::ErrorKind::WriteZero,
                    "failed to write to serial port",
                ));
            }
            Ok(n) => {
                offset += n;
                backpressure_started = None;
            }
            Err(ref e)
                if matches!(
                    e.kind(),
                    io::ErrorKind::TimedOut | io::ErrorKind::WouldBlock
                ) =>
            {
                let blocked_since = *backpressure_started.get_or_insert_with(Instant::now);
                if blocked_since.elapsed() >= SERIAL_WRITE_BACKPRESSURE_LIMIT {
                    return Err(io::Error::new(
                        io::ErrorKind::TimedOut,
                        "serial write backpressure limit exceeded",
                    ));
                }
                if stop.load(Ordering::Relaxed) || tcp_peer_disconnected()? {
                    stop.store(true, Ordering::Relaxed);
                    return Ok(());
                }
                thread::sleep(Duration::from_millis(1));
            }
            Err(e) => return Err(e),
        }
    }

    Ok(())
}

fn write_serial_all_with_retry_and_pacing(
    serial: &mut dyn SerialPort,
    data: &[u8],
    stop: &AtomicBool,
    mut tcp_peer_disconnected: impl FnMut() -> io::Result<bool>,
    chunk_bytes: usize,
    delay: Duration,
    mut sleep: impl FnMut(Duration),
) -> io::Result<()> {
    let chunk_bytes = chunk_bytes.max(1);
    let mut offset = 0usize;

    while offset < data.len() {
        let end = (offset + chunk_bytes).min(data.len());
        write_serial_all_with_retry(serial, &data[offset..end], stop, || {
            tcp_peer_disconnected()
        })?;
        offset = end;

        if offset < data.len() && !stop.load(Ordering::Relaxed) && !delay.is_zero() {
            sleep(delay);
        }
    }

    Ok(())
}

fn tcp_peer_disconnected(tcp: &TcpStream) -> io::Result<bool> {
    let mut buf = [0u8; 1];

    match tcp.peek(&mut buf) {
        Ok(0) => Ok(true),
        Ok(_) => Ok(false),
        Err(ref e)
            if matches!(
                e.kind(),
                io::ErrorKind::WouldBlock | io::ErrorKind::TimedOut
            ) =>
        {
            Ok(false)
        }
        Err(ref e)
            if matches!(
                e.kind(),
                io::ErrorKind::ConnectionAborted
                    | io::ErrorKind::ConnectionReset
                    | io::ErrorKind::NotConnected
                    | io::ErrorKind::BrokenPipe
            ) =>
        {
            Ok(true)
        }
        Err(e) => Err(e),
    }
}

fn copy_tcp_to_serial(
    mut tcp: TcpStream,
    mut serial: Box<dyn SerialPort>,
    stop: Arc<AtomicBool>,
    log: BridgeLog,
) -> io::Result<()> {
    tcp.set_read_timeout(Some(Duration::from_millis(1)))?;

    let mut buf = [0u8; 8192];
    let mut total = 0usize;
    let result = loop {
        if stop.load(Ordering::Relaxed) {
            break Ok(());
        }

        match tcp.read(&mut buf) {
            Ok(0) => {
                log.debug(format_args!("tcp->serial eof total={total}"));
                break Ok(());
            }
            Ok(n) => {
                total += n;
                log.debug(format_args!("tcp->serial read bytes={n} total={total}"));
                log.debug(format_args!("tcp->serial data {}", hex_preview(&buf[..n])));
                if let Err(e) = write_serial_all_with_retry_and_pacing(
                    &mut *serial,
                    &buf[..n],
                    &stop,
                    || tcp_peer_disconnected(&tcp),
                    DEFAULT_SERIAL_WRITE_CHUNK_BYTES,
                    DEFAULT_SERIAL_WRITE_DELAY,
                    thread::sleep,
                ) {
                    break Err(e);
                }
                log.debug(format_args!("tcp->serial wrote bytes={n} total={total}"));
            }
            Err(ref e)
                if matches!(
                    e.kind(),
                    io::ErrorKind::WouldBlock | io::ErrorKind::TimedOut
                ) =>
            {
                thread::sleep(Duration::from_millis(1))
            }
            Err(e) => break Err(e),
        }
    };

    stop.store(true, Ordering::Relaxed);
    let _ = tcp.shutdown(Shutdown::Both);
    result
}

fn copy_serial_to_tcp(
    mut serial: Box<dyn SerialPort>,
    mut tcp: TcpStream,
    stop: Arc<AtomicBool>,
    log: BridgeLog,
) -> io::Result<()> {
    let mut buf = [0u8; 8192];
    let mut total = 0usize;
    let result = loop {
        if stop.load(Ordering::Relaxed) {
            break Ok(());
        }

        match serial.read(&mut buf) {
            Ok(0) => thread::sleep(Duration::from_millis(1)),
            Ok(n) => {
                total += n;
                log.debug(format_args!("serial->tcp read bytes={n} total={total}"));
                log.debug(format_args!("serial->tcp data {}", hex_preview(&buf[..n])));
                if let Err(e) = tcp.write_all(&buf[..n]) {
                    break Err(e);
                }
                log.debug(format_args!("serial->tcp wrote bytes={n} total={total}"));
            }
            Err(ref e) if e.kind() == io::ErrorKind::TimedOut => {
                if stop.load(Ordering::Relaxed) {
                    break Ok(());
                }
                thread::sleep(Duration::from_millis(1));
            }
            Err(e) => break Err(e),
        }
    };

    stop.store(true, Ordering::Relaxed);
    let _ = tcp.shutdown(Shutdown::Both);
    result
}

fn log_copy_result(direction: &str, result: thread::Result<io::Result<()>>) {
    match result {
        Ok(Ok(())) => eprintln!("{direction} copy stopped"),
        Ok(Err(e)) => eprintln!("{direction} copy error: {e}"),
        Err(panic) => {
            if let Some(message) = panic.downcast_ref::<&str>() {
                eprintln!("{direction} copy panicked: {message}");
            } else if let Some(message) = panic.downcast_ref::<String>() {
                eprintln!("{direction} copy panicked: {message}");
            } else {
                eprintln!("{direction} copy panicked");
            }
        }
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
    log.info(format_args!(
        "serial write pacing chunk={} delay_ms={}",
        DEFAULT_SERIAL_WRITE_CHUNK_BYTES,
        DEFAULT_SERIAL_WRITE_DELAY.as_millis()
    ));
    let serial = serialport::new(&args.serial, args.baud)
        .timeout(Duration::from_millis(1))
        .open()?;
    log.debug("serial opened");
    send_bridge_reset_from_handle(&*serial, log)?;

    for stream in listener.incoming() {
        let stream = stream?;
        stream.set_nodelay(true)?;
        let peer = stream.peer_addr().ok();
        log.info(format_args!("client connected peer={peer:?}"));

        let serial_a = serial.try_clone()?;
        let serial_b = serial_a.try_clone()?;

        let tcp_a = stream.try_clone()?;
        let tcp_b = stream;
        let stop = Arc::new(AtomicBool::new(false));

        let stop_a = Arc::clone(&stop);
        let log_a = log;
        let t1 = thread::spawn(move || copy_tcp_to_serial(tcp_a, serial_a, stop_a, log_a));
        let stop_b = Arc::clone(&stop);
        let log_b = log;
        let t2 = thread::spawn(move || copy_serial_to_tcp(serial_b, tcp_b, stop_b, log_b));

        log_copy_result("tcp-to-serial", t1.join());
        log_copy_result("serial-to-tcp", t2.join());
        log.info("client disconnected");
        send_bridge_reset_from_handle(&*serial, log)?;
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
        ErrorForever(io::ErrorKind),
    }

    struct FakeSerialPort {
        actions: VecDeque<WriteAction>,
        written: Vec<u8>,
    }

    impl FakeSerialPort {
        fn new(actions: impl IntoIterator<Item = WriteAction>) -> Self {
            Self {
                actions: actions.into_iter().collect(),
                written: Vec::new(),
            }
        }
    }

    impl Read for FakeSerialPort {
        fn read(&mut self, _buf: &mut [u8]) -> io::Result<usize> {
            Err(io::Error::new(io::ErrorKind::WouldBlock, "not readable"))
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
                    Ok(n)
                }
                WriteAction::Error(kind) => Err(io::Error::new(kind, "write blocked")),
                WriteAction::ErrorForever(kind) => {
                    self.actions.push_front(WriteAction::ErrorForever(kind));
                    Err(io::Error::new(kind, "write blocked"))
                }
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

    #[test]
    fn default_baud_matches_firmware_default() {
        let args = Args::try_parse_from(["mc-uart-bridge", "--serial", "loop"]).unwrap();
        assert_eq!(args.baud, DEFAULT_BAUD);
    }

    #[test]
    fn baud_can_be_overridden() {
        let args = Args::try_parse_from([
            "mc-uart-bridge",
            "--serial",
            "loop",
            "--baud",
            "921600",
        ])
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
    fn send_bridge_reset_writes_magic_frame() {
        let mut serial = FakeSerialPort::new(BRIDGE_RESET_FRAME.map(|_| WriteAction::Write(1)));
        let log = BridgeLog { verbose: false };

        send_bridge_reset(&mut serial, log).unwrap();

        assert_eq!(serial.written, BRIDGE_RESET_FRAME);
    }

    #[test]
    fn serial_write_retries_timeouts_after_partial_progress_without_duplicates() {
        let stop = AtomicBool::new(false);
        let mut serial = FakeSerialPort::new([
            WriteAction::Write(2),
            WriteAction::Error(io::ErrorKind::TimedOut),
            WriteAction::Write(1),
            WriteAction::Error(io::ErrorKind::WouldBlock),
            WriteAction::Write(2),
        ]);

        write_serial_all_with_retry(&mut serial, b"abcde", &stop, || Ok(false)).unwrap();

        assert_eq!(serial.written, b"abcde");
    }

    #[test]
    fn serial_write_paces_trace_firmware_one_byte_at_a_time() {
        use std::cell::RefCell;

        let stop = AtomicBool::new(false);
        let mut serial = FakeSerialPort::new([
            WriteAction::Write(1),
            WriteAction::Write(1),
            WriteAction::Write(1),
        ]);
        let sleeps = RefCell::new(Vec::new());

        write_serial_all_with_retry_and_pacing(
            &mut serial,
            b"abc",
            &stop,
            || Ok(false),
            DEFAULT_SERIAL_WRITE_CHUNK_BYTES,
            DEFAULT_SERIAL_WRITE_DELAY,
            |delay| sleeps.borrow_mut().push(delay),
        )
        .unwrap();

        assert_eq!(serial.written, b"abc");
        assert_eq!(
            sleeps.into_inner(),
            vec![DEFAULT_SERIAL_WRITE_DELAY, DEFAULT_SERIAL_WRITE_DELAY]
        );
    }

    #[test]
    fn serial_write_returns_write_zero_on_zero_length_progress() {
        let stop = AtomicBool::new(false);
        let mut serial = FakeSerialPort::new([WriteAction::Write(0)]);

        let err = write_serial_all_with_retry(&mut serial, b"a", &stop, || Ok(false)).unwrap_err();

        assert_eq!(err.kind(), io::ErrorKind::WriteZero);
    }

    #[test]
    fn tcp_to_serial_stops_retrying_serial_write_after_tcp_peer_disconnects() {
        use std::net::TcpListener;
        use std::sync::mpsc;

        let listener = TcpListener::bind("127.0.0.1:0").unwrap();
        let addr = listener.local_addr().unwrap();
        let mut client = TcpStream::connect(addr).unwrap();
        let (server, _) = listener.accept().unwrap();
        let stop = Arc::new(AtomicBool::new(false));
        let serial = Box::new(FakeSerialPort::new([
            WriteAction::Write(1),
            WriteAction::ErrorForever(io::ErrorKind::TimedOut),
        ]));
        let (done_tx, done_rx) = mpsc::channel();
        let log = BridgeLog { verbose: false };

        let worker_stop = Arc::clone(&stop);
        thread::spawn(move || {
            let result = copy_tcp_to_serial(server, serial, worker_stop, log);
            done_tx.send(result).unwrap();
        });

        client.write_all(b"abc").unwrap();
        drop(client);

        let result = done_rx
            .recv_timeout(Duration::from_millis(250))
            .expect("tcp-to-serial thread did not stop after peer disconnect");
        result.unwrap();
        assert!(stop.load(Ordering::Relaxed));
    }

    #[test]
    fn tcp_to_serial_stops_when_peer_closes_with_buffered_tcp_data_and_serial_stays_blocked() {
        use std::net::TcpListener;
        use std::sync::mpsc;

        let listener = TcpListener::bind("127.0.0.1:0").unwrap();
        let addr = listener.local_addr().unwrap();
        let mut client = TcpStream::connect(addr).unwrap();
        let (server, _) = listener.accept().unwrap();
        let stop = Arc::new(AtomicBool::new(false));
        let serial = Box::new(FakeSerialPort::new([
            WriteAction::Write(16),
            WriteAction::ErrorForever(io::ErrorKind::TimedOut),
        ]));
        let (done_tx, done_rx) = mpsc::channel();
        let log = BridgeLog { verbose: false };

        let worker_stop = Arc::clone(&stop);
        thread::spawn(move || {
            let result = copy_tcp_to_serial(server, serial, worker_stop, log);
            done_tx.send(result).unwrap();
        });

        client.write_all(&vec![b'x'; 8192 * 2 + 1]).unwrap();
        drop(client);

        let result = done_rx
            .recv_timeout(Duration::from_millis(1500))
            .expect("tcp-to-serial thread stayed pinned behind buffered TCP data");
        assert_eq!(result.unwrap_err().kind(), io::ErrorKind::TimedOut);
        assert!(stop.load(Ordering::Relaxed));
    }
}
