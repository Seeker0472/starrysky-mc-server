pub const MAGIC: [u8; 2] = [0x4d, 0x55];
pub const VERSION: u8 = 1;
pub const DEFAULT_PAYLOAD: usize = 512;
pub const FIRMWARE_PAYLOAD_CAP: usize = 512;
pub const INITIAL_CREDIT: u16 = 512;

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum FrameType {
    Hello,
    Ready,
    DataC2m,
    DataM2c,
    Credit,
    Reset,
    ResetAck,
    Error,
    Ping,
    Pong,
    Unknown(u8),
}

#[derive(Debug, Clone, PartialEq, Eq)]
pub struct Frame {
    pub frame_type: FrameType,
    pub seq: u8,
    pub payload: Vec<u8>,
}

impl FrameType {
    pub fn to_u8(self) -> u8 {
        match self {
            Self::Hello => 0x01,
            Self::Ready => 0x02,
            Self::DataC2m => 0x03,
            Self::DataM2c => 0x04,
            Self::Credit => 0x05,
            Self::Reset => 0x06,
            Self::ResetAck => 0x07,
            Self::Error => 0x08,
            Self::Ping => 0x09,
            Self::Pong => 0x0a,
            Self::Unknown(raw) => raw,
        }
    }

    fn from_u8(value: u8) -> Self {
        match value {
            0x01 => Self::Hello,
            0x02 => Self::Ready,
            0x03 => Self::DataC2m,
            0x04 => Self::DataM2c,
            0x05 => Self::Credit,
            0x06 => Self::Reset,
            0x07 => Self::ResetAck,
            0x08 => Self::Error,
            0x09 => Self::Ping,
            0x0a => Self::Pong,
            raw => Self::Unknown(raw),
        }
    }
}

impl From<FrameType> for u8 {
    fn from(frame_type: FrameType) -> Self {
        frame_type.to_u8()
    }
}

pub fn crc16(data: &[u8]) -> u16 {
    let mut crc = 0xffffu16;

    for byte in data {
        crc ^= u16::from(*byte) << 8;
        for _ in 0..8 {
            if crc & 0x8000 != 0 {
                crc = (crc << 1) ^ 0x1021;
            } else {
                crc <<= 1;
            }
        }
    }

    crc
}

pub fn encode(frame_type: FrameType, seq: u8, payload: &[u8]) -> Option<Vec<u8>> {
    if payload.len() > FIRMWARE_PAYLOAD_CAP {
        return None;
    }
    if matches!(frame_type, FrameType::Unknown(_)) {
        return None;
    }

    let payload_len = u16::try_from(payload.len()).ok()?;
    let mut encoded = Vec::with_capacity(2 + 1 + 1 + 2 + payload.len() + 2);
    encoded.extend_from_slice(&MAGIC);
    encoded.push(frame_type.to_u8());
    encoded.push(seq);
    encoded.extend_from_slice(&payload_len.to_le_bytes());
    encoded.extend_from_slice(payload);
    let crc = crc16(&encoded[2..]);
    encoded.extend_from_slice(&crc.to_le_bytes());
    Some(encoded)
}

pub fn hello_payload(requested_payload: u16, desired_credit: u16) -> [u8; 5] {
    let requested_payload = requested_payload.to_le_bytes();
    let desired_credit = desired_credit.to_le_bytes();
    [
        VERSION,
        requested_payload[0],
        requested_payload[1],
        desired_credit[0],
        desired_credit[1],
    ]
}

pub fn parse_ready(payload: &[u8]) -> Option<(u16, u16)> {
    if payload.len() != 5 || payload[0] != VERSION {
        return None;
    }

    Some((
        u16::from_le_bytes([payload[1], payload[2]]),
        u16::from_le_bytes([payload[3], payload[4]]),
    ))
}

pub fn parse_credit(payload: &[u8]) -> Option<u16> {
    if payload.len() != 2 {
        return None;
    }

    Some(u16::from_le_bytes([payload[0], payload[1]]))
}

#[derive(Default)]
pub struct Decoder {
    buffer: Vec<u8>,
}

impl Decoder {
    pub fn new() -> Self {
        Self::default()
    }

    pub fn feed(&mut self, data: &[u8]) -> Vec<Frame> {
        const HEADER_LEN: usize = 6;
        const CRC_LEN: usize = 2;

        self.buffer.extend_from_slice(data);
        let mut frames = Vec::new();

        loop {
            let Some(magic_start) = self
                .buffer
                .windows(MAGIC.len())
                .position(|window| window == MAGIC)
            else {
                let keep = self.buffer.last().copied() == Some(MAGIC[0]);
                let last = self.buffer.last().copied();
                self.buffer.clear();
                if keep {
                    if let Some(byte) = last {
                        self.buffer.push(byte);
                    }
                }
                break;
            };

            if magic_start > 0 {
                self.buffer.drain(..magic_start);
            }

            if self.buffer.len() < HEADER_LEN {
                break;
            }

            let payload_len = u16::from_le_bytes([self.buffer[4], self.buffer[5]]) as usize;
            if payload_len > FIRMWARE_PAYLOAD_CAP {
                self.buffer.drain(..1);
                continue;
            }

            let frame_len = HEADER_LEN + payload_len + CRC_LEN;
            if self.buffer.len() < frame_len {
                break;
            }

            let crc_offset = HEADER_LEN + payload_len;
            let expected_crc =
                u16::from_le_bytes([self.buffer[crc_offset], self.buffer[crc_offset + 1]]);
            let actual_crc = crc16(&self.buffer[2..crc_offset]);

            if actual_crc != expected_crc {
                self.buffer.drain(..1);
                continue;
            }

            frames.push(Frame {
                frame_type: FrameType::from_u8(self.buffer[2]),
                seq: self.buffer[3],
                payload: self.buffer[HEADER_LEN..crc_offset].to_vec(),
            });

            self.buffer.drain(..frame_len);
        }

        frames
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn constants_match_firmware_contract() {
        assert_eq!(MAGIC, [0x4d, 0x55]);
        assert_eq!(VERSION, 1);
        assert_eq!(DEFAULT_PAYLOAD, 512);
        assert_eq!(FIRMWARE_PAYLOAD_CAP, 512);
        assert_eq!(INITIAL_CREDIT, 512);
    }

    #[test]
    fn crc_matches_c_vector() {
        let bytes = [FrameType::Hello.to_u8(), 0, 5, 0, 1, 0x00, 0x02, 0x00, 0x02];
        assert_eq!(crc16(&bytes), 0x6e85);
    }

    #[test]
    fn encode_hello_exact_bytes() {
        let payload = [1, 0x00, 0x02, 0x00, 0x02];
        assert_eq!(
            encode(FrameType::Hello, 0, &payload).unwrap(),
            vec![
                0x4d, 0x55, 0x01, 0x00, 0x05, 0x00, 0x01, 0x00, 0x02, 0x00, 0x02, 0x85,
                0x6e,
            ]
        );
    }

    #[test]
    fn decoder_accepts_split_frame() {
        let encoded = encode(FrameType::DataC2m, 7, b"abc").unwrap();
        let mut decoder = Decoder::new();
        assert!(decoder.feed(&encoded[..4]).is_empty());
        let frames = decoder.feed(&encoded[4..]);
        assert_eq!(
            frames,
            vec![Frame {
                frame_type: FrameType::DataC2m,
                seq: 7,
                payload: b"abc".to_vec(),
            }]
        );
    }

    #[test]
    fn decoder_resyncs_after_junk_and_bad_crc() {
        let mut bad = encode(FrameType::DataC2m, 1, b"x").unwrap();
        let last = bad.len() - 1;
        bad[last] ^= 0xff;
        let good = encode(FrameType::Ready, 0, &[1, 64, 0, 0, 2]).unwrap();
        let mut stream = vec![0, 0x4d, 0];
        stream.extend_from_slice(&bad);
        stream.extend_from_slice(&good);
        let mut decoder = Decoder::new();
        let frames = decoder.feed(&stream);
        assert_eq!(frames.len(), 1);
        assert_eq!(frames[0].frame_type, FrameType::Ready);
    }

    #[test]
    fn decoder_surfaces_unknown_valid_frame_type() {
        let payload = b"unexpected";
        let payload_len = payload.len() as u16;
        let mut encoded = Vec::new();
        encoded.extend_from_slice(&MAGIC);
        encoded.push(0xfe);
        encoded.push(9);
        encoded.extend_from_slice(&payload_len.to_le_bytes());
        encoded.extend_from_slice(payload);
        let crc = crc16(&encoded[2..]);
        encoded.extend_from_slice(&crc.to_le_bytes());

        let mut decoder = Decoder::new();
        let frames = decoder.feed(&encoded);
        assert_eq!(
            frames,
            vec![Frame {
                frame_type: FrameType::Unknown(0xfe),
                seq: 9,
                payload: payload.to_vec(),
            }]
        );
    }

    #[test]
    fn decoder_returns_multiple_valid_frames_from_one_feed() {
        let first = encode(FrameType::Ping, 1, b"one").unwrap();
        let second = encode(FrameType::Pong, 2, b"two").unwrap();
        let mut stream = first;
        stream.extend_from_slice(&second);

        let mut decoder = Decoder::new();
        let frames = decoder.feed(&stream);
        assert_eq!(
            frames,
            vec![
                Frame {
                    frame_type: FrameType::Ping,
                    seq: 1,
                    payload: b"one".to_vec(),
                },
                Frame {
                    frame_type: FrameType::Pong,
                    seq: 2,
                    payload: b"two".to_vec(),
                },
            ]
        );
    }

    #[test]
    fn decoder_resyncs_after_over_cap_length_frame() {
        let mut over_cap = Vec::new();
        over_cap.extend_from_slice(&MAGIC);
        over_cap.push(FrameType::DataC2m.to_u8());
        over_cap.push(3);
        over_cap.extend_from_slice(&((FIRMWARE_PAYLOAD_CAP + 1) as u16).to_le_bytes());
        over_cap.extend_from_slice(&[0xaa, 0xbb, 0xcc]);

        let good = encode(FrameType::Ready, 0, &[1, 64, 0, 0, 2]).unwrap();
        let mut stream = over_cap;
        stream.extend_from_slice(&good);

        let mut decoder = Decoder::new();
        let frames = decoder.feed(&stream);
        assert_eq!(
            frames,
            vec![Frame {
                frame_type: FrameType::Ready,
                seq: 0,
                payload: vec![1, 64, 0, 0, 2],
            }]
        );
    }

    #[test]
    fn encode_rejects_payload_over_cap() {
        assert!(encode(FrameType::DataC2m, 0, &vec![0; FIRMWARE_PAYLOAD_CAP + 1]).is_none());
    }

    #[test]
    fn encode_rejects_unknown_frame_type() {
        assert!(encode(FrameType::Unknown(0xfe), 0, b"unexpected").is_none());
    }
}
