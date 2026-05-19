use crate::link;
use serialport::{ClearBuffer, DataBits, FlowControl, Parity, SerialPort, StopBits};
use std::collections::VecDeque;
use std::io::{self, Read, Write};
use std::thread;
use std::time::Duration;

pub(crate) enum WriteAction {
    Write(usize),
    Error(io::ErrorKind),
}

pub(crate) struct FakeSerialPort {
    pub(crate) actions: VecDeque<WriteAction>,
    pub(crate) readable: VecDeque<u8>,
    pub(crate) written: Vec<u8>,
    pub(crate) max_read_len: Option<usize>,
    pub(crate) read_chunk_lens: VecDeque<usize>,
    pub(crate) readable_after_written_bytes: VecDeque<(usize, Vec<u8>)>,
    pub(crate) auto_reset_ready_payload: Option<Vec<u8>>,
    pub(crate) read_delay: Option<Duration>,
    pub(crate) empty_read_error: io::ErrorKind,
    pub(crate) clone_error: Option<io::ErrorKind>,
}

impl FakeSerialPort {
    pub(crate) fn new(actions: impl IntoIterator<Item = WriteAction>) -> Self {
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
            clone_error: None,
        }
    }

    pub(crate) fn with_readable(mut self, readable: impl IntoIterator<Item = u8>) -> Self {
        self.readable = readable.into_iter().collect();
        self
    }

    pub(crate) fn with_readable_after_written_bytes(
        mut self,
        written_len: usize,
        readable: impl IntoIterator<Item = u8>,
    ) -> Self {
        self.readable_after_written_bytes
            .push_back((written_len, readable.into_iter().collect()));
        self
    }

    pub(crate) fn with_max_read_len(mut self, max_read_len: usize) -> Self {
        self.max_read_len = Some(max_read_len);
        self
    }

    pub(crate) fn with_auto_reset_ready(mut self, ready_payload: &[u8]) -> Self {
        self.auto_reset_ready_payload = Some(ready_payload.to_vec());
        self
    }

    pub(crate) fn with_read_delay(mut self, delay: Duration) -> Self {
        self.read_delay = Some(delay);
        self
    }

    pub(crate) fn with_empty_read_error(mut self, kind: io::ErrorKind) -> Self {
        self.empty_read_error = kind;
        self
    }

    pub(crate) fn with_clone_error(mut self, kind: io::ErrorKind) -> Self {
        self.clone_error = Some(kind);
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
        if let Some(kind) = self.clone_error {
            return Err(serialport::Error::new(
                serialport::ErrorKind::Io(kind),
                "clone failed",
            ));
        }
        Ok(Box::new(
            Self::new([]).with_readable(self.readable.iter().copied()),
        ))
    }

    fn set_break(&self) -> serialport::Result<()> {
        Ok(())
    }

    fn clear_break(&self) -> serialport::Result<()> {
        Ok(())
    }
}
