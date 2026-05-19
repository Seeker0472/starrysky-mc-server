use crate::link;
use crate::rate::RateController;
use std::collections::VecDeque;
use std::io;
use std::time::{Duration, Instant};

pub(crate) struct OutstandingC2m {
    pub(crate) seq: u16,
    pub(crate) payload_len: usize,
    pub(crate) encoded: Vec<u8>,
    pub(crate) sent_at: Instant,
    pub(crate) retries: u8,
}

pub(crate) struct C2mSender {
    pub(crate) pending_tcp: VecDeque<u8>,
    pub(crate) outstanding: Option<OutstandingC2m>,
    pub(crate) next_seq: u16,
    pub(crate) negotiated_payload: usize,
    pub(crate) credit: u16,
}

impl C2mSender {
    pub(crate) fn new(negotiated_payload: usize, credit: u16) -> Self {
        Self {
            pending_tcp: VecDeque::new(),
            outstanding: None,
            next_seq: 0,
            negotiated_payload,
            credit,
        }
    }

    pub(crate) fn push_tcp_bytes(&mut self, data: &[u8]) {
        self.pending_tcp.extend(data);
    }

    pub(crate) fn pending_tcp_len(&self) -> usize {
        self.pending_tcp.len()
    }

    pub(crate) fn reset_link_state_preserving_pending(&mut self, negotiated_payload: usize, credit: u16) {
        self.outstanding = None;
        self.next_seq = 0;
        self.negotiated_payload = negotiated_payload;
        self.credit = credit;
    }

    pub(crate) fn next_frame_to_send(&mut self) -> io::Result<Option<link::Frame>> {
        let Some(payload_len) = self.next_payload_len() else {
            return Ok(None);
        };
        if self.next_seq == u16::MAX {
            return Err(io::Error::new(
                io::ErrorKind::InvalidData,
                "C2M sequence exhausted; reset required",
            ));
        }
        let payload = self.pending_tcp.iter().take(payload_len).copied().collect();

        Ok(Some(link::Frame {
            frame_type: link::FrameType::DataC2m,
            flags: 0,
            seq: self.next_seq,
            ack: 0,
            payload,
        }))
    }

    pub(crate) fn mark_sent(&mut self, frame: &link::Frame) -> io::Result<()> {
        let Some(payload_len) = self.next_payload_len() else {
            return Ok(());
        };
        if frame.seq != self.next_seq || frame.payload.len() != payload_len {
            return Err(io::Error::new(
                io::ErrorKind::InvalidInput,
                "DATA_C2M frame does not match sender state",
            ));
        }
        let encoded = encode_data_c2m_frame(frame)?;

        self.credit = self.credit.saturating_sub(payload_len as u16);
        self.outstanding = Some(OutstandingC2m {
            seq: self.next_seq,
            payload_len,
            encoded,
            sent_at: Instant::now(),
            retries: 0,
        });
        self.next_seq = self.next_seq.wrapping_add(1);
        Ok(())
    }

    pub(crate) fn handle_ack(&mut self, ack: u16, credit: u16) -> io::Result<()> {
        self.credit = credit;

        let Some(outstanding) = self.outstanding.as_ref() else {
            return Ok(());
        };
        if ack != outstanding.seq.wrapping_add(1) {
            return Ok(());
        }

        let payload_len = outstanding.payload_len;
        if self.pending_tcp.len() < payload_len {
            return Err(io::Error::new(
                io::ErrorKind::InvalidData,
                "ACK_C2M advanced beyond pending DATA_C2M bytes",
            ));
        }

        for _ in 0..payload_len {
            self.pending_tcp.pop_front();
        }
        self.outstanding = None;
        Ok(())
    }

    pub(crate) fn has_outstanding(&self) -> bool {
        self.outstanding.is_some()
    }

    pub(crate) fn retransmit_if_due(&mut self, now: Instant, timeout: Duration) -> Option<Vec<u8>> {
        let outstanding = self.outstanding.as_ref()?;
        let Some(elapsed) = now.checked_duration_since(outstanding.sent_at) else {
            return None;
        };
        if elapsed < timeout {
            return None;
        }

        Some(outstanding.encoded.clone())
    }

    pub(crate) fn mark_retransmitted(&mut self, now: Instant) {
        let Some(outstanding) = self.outstanding.as_mut() else {
            return;
        };
        outstanding.sent_at = now;
        outstanding.retries = outstanding.retries.saturating_add(1);
    }

    pub(crate) fn next_payload_len(&self) -> Option<usize> {
        if self.outstanding.is_some() || self.pending_tcp.is_empty() || self.credit == 0 {
            return None;
        }

        let payload_cap = self
            .negotiated_payload
            .max(1)
            .min(link::FIRMWARE_PAYLOAD_CAP);
        let payload_len = self
            .pending_tcp
            .len()
            .min(payload_cap)
            .min(self.credit as usize);
        (payload_len > 0).then_some(payload_len)
    }
}

pub(crate) fn handle_ack_c2m(sender: &mut C2mSender, frame: &link::Frame) -> io::Result<()> {
    let Some(credit) = link::ack_credit(&frame.payload) else {
        return Err(io::Error::new(
            io::ErrorKind::InvalidData,
            "invalid ACK_C2M payload",
        ));
    };
    sender.handle_ack(frame.ack, credit)
}

pub(crate) fn handle_ack_c2m_with_rate(
    sender: &mut C2mSender,
    rate_controller: &mut RateController,
    frame: &link::Frame,
) -> io::Result<()> {
    let had_outstanding = sender.has_outstanding();
    handle_ack_c2m(sender, frame)?;
    if had_outstanding && !sender.has_outstanding() {
        rate_controller.record_success();
    }
    Ok(())
}

pub(crate) fn encode_data_c2m_frame(frame: &link::Frame) -> io::Result<Vec<u8>> {
    link::encode(frame.frame_type, frame.seq, frame.ack, &frame.payload).ok_or_else(|| {
        io::Error::new(
            io::ErrorKind::InvalidInput,
            "failed to encode DATA_C2M frame",
        )
    })
}
