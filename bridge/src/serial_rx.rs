use crate::config::IO_BUF_LEN;
use serialport::SerialPort;
use std::io::{self, Read};
use std::sync::atomic::{AtomicBool, Ordering};
use std::sync::mpsc::{self, Receiver};
use std::sync::Arc;
use std::thread;

#[derive(Debug)]
pub(crate) enum SerialRxEvent {
    Bytes(Vec<u8>),
    Error(io::Error),
}

pub(crate) struct SerialRxWorker {
    rx: Receiver<SerialRxEvent>,
    stop: Arc<AtomicBool>,
    join: Option<thread::JoinHandle<()>>,
}

impl SerialRxWorker {
    pub(crate) fn receiver(&self) -> &Receiver<SerialRxEvent> {
        &self.rx
    }

    pub(crate) fn stop(&mut self) {
        self.stop.store(true, Ordering::Relaxed);
        if let Some(join) = self.join.take() {
            let _ = join.join();
        }
    }
}

impl Drop for SerialRxWorker {
    fn drop(&mut self) {
        self.stop();
    }
}

pub(crate) fn spawn_serial_rx_worker(serial: &mut dyn SerialPort) -> io::Result<SerialRxWorker> {
    let mut rx_serial = serial.try_clone().map_err(|error| {
        let kind = match error.kind() {
            serialport::ErrorKind::NoDevice => io::ErrorKind::NotFound,
            serialport::ErrorKind::InvalidInput => io::ErrorKind::InvalidInput,
            serialport::ErrorKind::Unknown => io::ErrorKind::Other,
            serialport::ErrorKind::Io(kind) => kind,
        };
        io::Error::new(
            kind,
            format!("failed to clone serial port for RX worker: {error}"),
        )
    })?;
    let (tx, rx) = mpsc::channel();
    let stop = Arc::new(AtomicBool::new(false));
    let worker_stop = Arc::clone(&stop);

    let join = thread::spawn(move || {
        let mut buf = [0u8; IO_BUF_LEN];
        while !worker_stop.load(Ordering::Relaxed) {
            match rx_serial.read(&mut buf) {
                Ok(0) => {}
                Ok(n) => {
                    if tx.send(SerialRxEvent::Bytes(buf[..n].to_vec())).is_err() {
                        break;
                    }
                }
                Err(ref e)
                    if matches!(
                        e.kind(),
                        io::ErrorKind::WouldBlock | io::ErrorKind::TimedOut
                    ) => {}
                Err(e) => {
                    let _ = tx.send(SerialRxEvent::Error(e));
                    break;
                }
            }
        }
    });

    Ok(SerialRxWorker {
        rx,
        stop,
        join: Some(join),
    })
}
