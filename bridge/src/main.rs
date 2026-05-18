mod link;

use clap::Parser;
use serialport::SerialPort;
use std::collections::VecDeque;
use std::io::{self, Read, Write};
use std::net::{TcpListener, TcpStream};
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
const DATA_FRAME_WRITE_CHUNK_BYTES: usize = 1;
const DATA_FRAME_WRITE_DELAY: Duration = Duration::from_millis(2);

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

fn encode_frame(frame_type: link::FrameType, seq: u8, payload: &[u8]) -> io::Result<Vec<u8>> {
    link::encode(frame_type, seq, payload).ok_or_else(|| {
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
    sleep: impl FnMut(Duration),
) -> io::Result<()> {
    write_frame_paced(
        serial,
        frame,
        DATA_FRAME_WRITE_CHUNK_BYTES,
        DATA_FRAME_WRITE_DELAY,
        sleep,
    )
}

fn send_link_reset(serial: &mut dyn SerialPort, log: BridgeLog) -> io::Result<()> {
    let frame = encode_frame(link::FrameType::Reset, 0, &[])?;
    log.debug(format_args!("link reset write bytes={}", frame.len()));
    log.debug(format_args!("link reset data {}", hex_preview(&frame)));
    write_control_frame(serial, &frame)
}

fn accept_m2c_sequence(frame: &link::Frame, expected: &mut u8, log: BridgeLog) -> bool {
    if frame.seq != *expected {
        log.info(format_args!(
            "DATA_M2C sequence mismatch expected={} got={}",
            *expected, frame.seq
        ));
        return false;
    }
    *expected = expected.wrapping_add(1);
    true
}

fn frame_tcp_payload_with_credit(
    input: &[u8],
    negotiated_payload: usize,
    credit: &mut u16,
    seq: &mut u8,
) -> Vec<Vec<u8>> {
    let payload_cap = negotiated_payload.max(1).min(link::FIRMWARE_PAYLOAD_CAP);
    let mut frames = Vec::new();
    let mut offset = 0usize;

    while offset < input.len() && *credit > 0 {
        let allowed = payload_cap.min(*credit as usize).min(input.len() - offset);
        if allowed == 0 {
            break;
        }
        let end = offset + allowed;
        let frame = link::encode(link::FrameType::DataC2m, *seq, &input[offset..end])
            .expect("payload cap guarantees DATA_C2M encodes");
        frames.push(frame);
        *credit -= allowed as u16;
        *seq = seq.wrapping_add(1);
        offset = end;
    }

    frames
}

fn configure_tcp_stream(tcp: &TcpStream) -> io::Result<()> {
    tcp.set_read_timeout(Some(TCP_READ_TIMEOUT))?;
    tcp.set_write_timeout(Some(TCP_WRITE_TIMEOUT))
}

fn normalize_ready(payload: u16, credit: u16) -> Option<(usize, u16)> {
    if payload == 0 || credit == 0 {
        return None;
    }
    Some((
        usize::from(payload).min(link::FIRMWARE_PAYLOAD_CAP),
        credit.min(link::INITIAL_CREDIT),
    ))
}

fn apply_link_credit(credit: &mut u16, credit_cap: u16, delta: u16) {
    *credit = credit.saturating_add(delta).min(credit_cap);
}

fn apply_link_ready_state(
    payload: u16,
    ready_credit: u16,
    negotiated_payload: &mut usize,
    credit: &mut u16,
    credit_cap: &mut u16,
    c2m_seq: &mut u8,
    m2c_seq_expected: &mut u8,
    pending_tcp: &mut VecDeque<u8>,
) -> bool {
    let Some((new_payload, new_credit)) = normalize_ready(payload, ready_credit) else {
        return false;
    };
    *negotiated_payload = new_payload;
    *credit = new_credit;
    *credit_cap = new_credit;
    *c2m_seq = 0;
    *m2c_seq_expected = 0;
    pending_tcp.clear();
    true
}

fn wait_for_ready(serial: &mut dyn SerialPort, log: BridgeLog) -> io::Result<(usize, u16)> {
    let hello_payload =
        link::hello_payload(link::DEFAULT_PAYLOAD as u16, link::INITIAL_CREDIT);
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
                for frame in decoder.feed(&buf[..n]) {
                    match frame.frame_type {
                        link::FrameType::Ready => {
                            let Some((payload, credit)) = link::parse_ready(&frame.payload) else {
                                log.info(format_args!(
                                    "link READY with invalid payload {}",
                                    hex_preview(&frame.payload)
                                ));
                                continue;
                            };
                            let Some((negotiated_payload, credit)) =
                                normalize_ready(payload, credit)
                            else {
                                log.info(format_args!(
                                    "link READY with invalid values payload={} credit={}",
                                    payload, credit
                                ));
                                continue;
                            };
                            log.info(format_args!(
                                "link ready payload={negotiated_payload} credit={credit}"
                            ));
                            return Ok((negotiated_payload, credit));
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

fn run_link_client(
    mut tcp: TcpStream,
    serial: &mut dyn SerialPort,
    log: BridgeLog,
) -> io::Result<()> {
    configure_tcp_stream(&tcp)?;

    send_link_reset(serial, log)?;
    let (mut negotiated_payload, mut credit) = wait_for_ready(serial, log)?;
    let mut credit_cap = credit;
    let mut decoder = link::Decoder::new();
    let mut pending_tcp = VecDeque::new();
    let mut c2m_seq = 0u8;
    let mut m2c_seq_expected = 0u8;
    let mut tcp_buf = [0u8; IO_BUF_LEN];
    let mut serial_buf = [0u8; IO_BUF_LEN];
    let mut tcp_read_total = 0usize;
    let mut tcp_to_serial_total = 0usize;
    let mut serial_to_tcp_total = 0usize;

    loop {
        if pending_tcp.len() < TCP_PENDING_LIMIT {
            let read_cap = (TCP_PENDING_LIMIT - pending_tcp.len()).min(tcp_buf.len());
            match tcp.read(&mut tcp_buf[..read_cap]) {
                Ok(0) => {
                    log.debug(format_args!("tcp eof total={tcp_to_serial_total}"));
                    let _ = send_link_reset(serial, log);
                    return Ok(());
                }
                Ok(n) => {
                    tcp_read_total += n;
                    pending_tcp.extend(&tcp_buf[..n]);
                    log.debug(format_args!(
                        "tcp read bytes={n} pending={} total_in={}",
                        pending_tcp.len(),
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

        while credit > 0 && !pending_tcp.is_empty() {
            let chunk_len = pending_tcp.len().min(credit as usize);
            let chunk: Vec<u8> = pending_tcp.iter().take(chunk_len).copied().collect();
            let frames = frame_tcp_payload_with_credit(
                &chunk,
                negotiated_payload,
                &mut credit,
                &mut c2m_seq,
            );

            if frames.is_empty() {
                return Err(io::Error::new(
                    io::ErrorKind::WriteZero,
                    "credit available but no DATA_C2M frame was produced",
                ));
            }

            for frame in frames {
                let payload_len = u16::from_le_bytes([frame[4], frame[5]]) as usize;
                if let Err(e) = write_data_frame_paced(serial, &frame, thread::sleep) {
                    let _ = send_link_reset(serial, log);
                    return Err(e);
                }
                for _ in 0..payload_len {
                    pending_tcp.pop_front();
                }
                tcp_to_serial_total += payload_len;
                log.debug(format_args!(
                    "DATA_C2M wrote bytes={payload_len} credit={credit} total={tcp_to_serial_total}"
                ));
            }
        }

        match serial.read(&mut serial_buf) {
            Ok(0) => {}
            Ok(n) => {
                log.debug(format_args!("serial link read bytes={n}"));
                for frame in decoder.feed(&serial_buf[..n]) {
                    match frame.frame_type {
                        link::FrameType::DataM2c => {
                            if !accept_m2c_sequence(&frame, &mut m2c_seq_expected, log) {
                                send_link_reset(serial, log)?;
                                let ready = wait_for_ready(serial, log)?;
                                negotiated_payload = ready.0;
                                credit = ready.1;
                                credit_cap = ready.1;
                                c2m_seq = 0;
                                m2c_seq_expected = 0;
                                pending_tcp.clear();
                                continue;
                            }
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
                        link::FrameType::Credit => {
                            if let Some(additional) = link::parse_credit(&frame.payload) {
                                apply_link_credit(&mut credit, credit_cap, additional);
                                log.debug(format_args!(
                                    "link credit add={additional} available={credit}"
                                ));
                            } else {
                                log.info(format_args!(
                                    "link CREDIT with invalid payload {}",
                                    hex_preview(&frame.payload)
                                ));
                            }
                        }
                        link::FrameType::Ready => {
                            if let Some((payload, ready_credit)) =
                                link::parse_ready(&frame.payload)
                            {
                                if apply_link_ready_state(
                                    payload,
                                    ready_credit,
                                    &mut negotiated_payload,
                                    &mut credit,
                                    &mut credit_cap,
                                    &mut c2m_seq,
                                    &mut m2c_seq_expected,
                                    &mut pending_tcp,
                                ) {
                                    log.info(format_args!(
                                        "link ready update payload={negotiated_payload} credit={credit}"
                                    ));
                                } else {
                                    log.info(format_args!(
                                        "link READY with invalid values payload={} credit={}",
                                        payload, ready_credit
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
                            let ready = wait_for_ready(serial, log)?;
                            negotiated_payload = ready.0;
                            credit = ready.1;
                            credit_cap = ready.1;
                            c2m_seq = 0;
                            m2c_seq_expected = 0;
                            pending_tcp.clear();
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
    }

    impl FakeSerialPort {
        fn new(actions: impl IntoIterator<Item = WriteAction>) -> Self {
            Self {
                actions: actions.into_iter().collect(),
                readable: VecDeque::new(),
                written: Vec::new(),
            }
        }

        fn with_readable(mut self, readable: impl IntoIterator<Item = u8>) -> Self {
            self.readable = readable.into_iter().collect();
            self
        }
    }

    impl Read for FakeSerialPort {
        fn read(&mut self, buf: &mut [u8]) -> io::Result<usize> {
            if self.readable.is_empty() {
                return Err(io::Error::new(io::ErrorKind::TimedOut, "not readable"));
            }

            let n = buf.len().min(self.readable.len());
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
    fn link_credit_splits_tcp_payload_and_subtracts_credit() {
        let mut credit = 100u16;
        let mut seq = 0u8;

        let frames =
            frame_tcp_payload_with_credit(&vec![0xaa; 150], 64, &mut credit, &mut seq);

        assert_eq!(frames.len(), 2);
        assert_eq!(frames[0][2], link::FrameType::DataC2m.to_u8());
        assert_eq!(u16::from_le_bytes([frames[0][4], frames[0][5]]), 64);
        assert_eq!(u16::from_le_bytes([frames[1][4], frames[1][5]]), 36);
        assert_eq!(credit, 0);
        assert_eq!(seq, 2);
    }

    #[test]
    fn link_credit_caps_frame_payload_to_available_credit_bytes() {
        let mut credit = 2u16;
        let mut seq = 250u8;

        let frames = frame_tcp_payload_with_credit(b"abcdef", 64, &mut credit, &mut seq);

        assert_eq!(frames.len(), 1);
        assert_eq!(frames[0][2], link::FrameType::DataC2m.to_u8());
        assert_eq!(frames[0][3], 250);
        assert_eq!(u16::from_le_bytes([frames[0][4], frames[0][5]]), 2);
        assert_eq!(&frames[0][6..8], b"ab");
        assert_eq!(credit, 0);
        assert_eq!(seq, 251);
    }

    #[test]
    fn link_credit_does_not_frame_when_zero() {
        let mut credit = 0u16;
        let mut seq = 7u8;

        let frames = frame_tcp_payload_with_credit(b"abc", 64, &mut credit, &mut seq);

        assert!(frames.is_empty());
        assert_eq!(credit, 0);
        assert_eq!(seq, 7);
    }

    #[test]
    fn normalize_ready_rejects_zero_payload_or_credit() {
        assert!(normalize_ready(0, 512).is_none());
        assert!(normalize_ready(64, 0).is_none());
    }

    #[test]
    fn link_ready_state_restarts_sequence_and_clears_pending_tcp() {
        let mut negotiated_payload = 32usize;
        let mut credit = 4u16;
        let mut credit_cap = 4u16;
        let mut c2m_seq = 9u8;
        let mut m2c_seq_expected = 7u8;
        let mut pending_tcp = VecDeque::from([1u8, 2, 3]);

        assert!(apply_link_ready_state(
            128,
            300,
            &mut negotiated_payload,
            &mut credit,
            &mut credit_cap,
            &mut c2m_seq,
            &mut m2c_seq_expected,
            &mut pending_tcp,
        ));

        assert_eq!(negotiated_payload, 128);
        assert_eq!(credit, 300);
        assert_eq!(credit_cap, 300);
        assert_eq!(c2m_seq, 0);
        assert_eq!(m2c_seq_expected, 0);
        assert!(pending_tcp.is_empty());
    }

    #[test]
    fn link_ready_state_restarts_m2c_sequence() {
        let mut negotiated_payload = 32usize;
        let mut credit = 4u16;
        let mut credit_cap = 4u16;
        let mut c2m_seq = 9u8;
        let mut m2c_seq_expected = 17u8;
        let mut pending_tcp = VecDeque::from([1u8, 2, 3]);

        assert!(apply_link_ready_state(
            128,
            300,
            &mut negotiated_payload,
            &mut credit,
            &mut credit_cap,
            &mut c2m_seq,
            &mut m2c_seq_expected,
            &mut pending_tcp,
        ));

        assert_eq!(m2c_seq_expected, 0);
    }

    #[test]
    fn m2c_sequence_rejects_duplicate_without_advancing() {
        let log = BridgeLog { verbose: false };
        let mut expected = 1u8;
        let frame = link::Frame {
            frame_type: link::FrameType::DataM2c,
            seq: 0,
            payload: b"stale".to_vec(),
        };

        assert!(!accept_m2c_sequence(&frame, &mut expected, log));
        assert_eq!(expected, 1);
    }

    #[test]
    fn m2c_sequence_accepts_expected_and_wraps() {
        let log = BridgeLog { verbose: false };
        let mut expected = 255u8;
        let frame = link::Frame {
            frame_type: link::FrameType::DataM2c,
            seq: 255,
            payload: b"ok".to_vec(),
        };

        assert!(accept_m2c_sequence(&frame, &mut expected, log));
        assert_eq!(expected, 0);
    }

    #[test]
    fn link_credit_is_capped_to_negotiated_credit() {
        let mut credit = 90u16;

        apply_link_credit(&mut credit, 100, 50);

        assert_eq!(credit, 100);
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
        let frame = link::encode(link::FrameType::Ping, 4, b"abc").unwrap();
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

        let frame =
            link::encode(link::FrameType::Hello, 0, &[link::VERSION, 0, 2, 0, 2]).unwrap();
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
    fn data_frame_write_paces_one_byte_at_a_time() {
        use std::cell::RefCell;

        let frame = link::encode(link::FrameType::DataC2m, 0, b"login").unwrap();
        let mut serial = FakeSerialPort::new(frame.iter().map(|_| WriteAction::Write(1)));
        let sleeps = RefCell::new(Vec::new());

        write_data_frame_paced(&mut serial, &frame, |delay| sleeps.borrow_mut().push(delay))
            .unwrap();

        assert_eq!(serial.written, frame);
        assert_eq!(sleeps.into_inner().len(), frame.len() - 1);
    }

    #[test]
    fn send_link_reset_writes_reset_frame() {
        let frame = link::encode(link::FrameType::Reset, 0, &[]).unwrap();
        let mut serial = FakeSerialPort::new(frame.iter().map(|_| WriteAction::Write(1)));
        let log = BridgeLog { verbose: false };

        send_link_reset(&mut serial, log).unwrap();

        let mut decoder = link::Decoder::new();
        let frames = decoder.feed(&serial.written);
        assert_eq!(frames.len(), 1);
        assert_eq!(frames[0].frame_type, link::FrameType::Reset);
        assert!(frames[0].payload.is_empty());
    }

    #[test]
    fn wait_for_ready_accepts_ready_frame() {
        let ready =
            link::encode(link::FrameType::Ready, 0, &[link::VERSION, 0, 2, 0, 2]).unwrap();
        let mut serial = FakeSerialPort::new([]).with_readable(ready);
        let log = BridgeLog { verbose: false };

        let (payload, credit) = wait_for_ready(&mut serial, log).unwrap();

        assert_eq!(payload, 512);
        assert_eq!(credit, 512);
        assert_eq!(&serial.written[..link::MAGIC.len()], link::MAGIC);
    }
}
