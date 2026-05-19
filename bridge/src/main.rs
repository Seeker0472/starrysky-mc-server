mod c2m;
mod config;
mod handshake;
mod link;
mod log;
mod rate;
mod rate_calibration;
mod serial_io;
mod serial_rx;
mod session;

#[cfg(test)]
mod test_support;
#[cfg(test)]
mod tests;

use clap::Parser;
use config::{DEFAULT_BAUD, DEFAULT_C2M_RETRANSMIT_TIMEOUT_MS};
use log::BridgeLog;
use std::io;
use std::net::TcpListener;
use std::time::Duration;

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

    #[arg(long, default_value_t = DEFAULT_C2M_RETRANSMIT_TIMEOUT_MS)]
    c2m_retransmit_timeout_ms: u64,
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
    let _ = handshake::wait_for_ready(&mut *serial, log)?;

    for stream in listener.incoming() {
        let stream = stream?;
        stream.set_nodelay(true)?;
        let peer = stream.peer_addr().ok();
        log.info(format_args!("client connected peer={peer:?}"));

        let c2m_retransmit_timeout = Duration::from_millis(args.c2m_retransmit_timeout_ms);
        match session::run_link_client(stream, &mut *serial, log, c2m_retransmit_timeout) {
            Ok(()) => log.info("client disconnected"),
            Err(e) if e.kind() == io::ErrorKind::InvalidData => return Err(e),
            Err(e) => log.info(format_args!("client link session error: {e}")),
        }
    }

    Ok(())
}
