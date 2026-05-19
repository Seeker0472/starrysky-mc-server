#[derive(Clone, Copy)]
pub(crate) struct BridgeLog {
    pub(crate) verbose: bool,
}

impl BridgeLog {
    pub(crate) fn info(self, message: impl std::fmt::Display) {
        eprintln!("[I] {message}");
    }

    pub(crate) fn debug(self, message: impl std::fmt::Display) {
        if self.verbose {
            eprintln!("[D] {message}");
        }
    }
}

pub(crate) fn hex_preview(data: &[u8]) -> String {
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
