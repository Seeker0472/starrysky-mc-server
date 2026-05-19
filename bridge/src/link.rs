pub const VERSION: u8 = 2;
pub const DELIMITER: u8 = 0;
pub const BODY_HEADER_LEN: usize = 9;
pub const CRC_LEN: usize = 2;
pub const MAX_ENCODED_LEN: usize = 512;
pub const MAX_PAYLOAD: usize = 497;
pub const DEFAULT_PAYLOAD: usize = MAX_PAYLOAD;
pub const FIRMWARE_PAYLOAD_CAP: usize = MAX_PAYLOAD;
pub const INITIAL_CREDIT: u16 = 512;
pub const SUPPORTED_RATE_MASK: u16 = 0x03ff;
pub const STARTUP_RATE_PROFILE: u8 = 0;

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum FrameType {
    Hello,
    Ready,
    DataC2m,
    AckC2m,
    DataM2c,
    RateProbe,
    RateProbeAck,
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
    pub flags: u8,
    pub seq: u16,
    pub ack: u16,
    pub payload: Vec<u8>,
}

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub struct ReadyPayload {
    pub negotiated_payload: u16,
    pub credit_cap: u16,
    pub initial_credit: u16,
    pub supported_rate_mask: u16,
    pub initial_rate_profile: u8,
    pub flags: u16,
}

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum DecodeError {
    Cobs,
    Crc,
    Version(u8),
    Length,
    PayloadTooLarge,
}

#[derive(Debug, Clone, PartialEq, Eq)]
pub enum DecodeEvent {
    Frame(Frame),
    Error(DecodeError),
}

impl FrameType {
    pub fn to_u8(self) -> u8 {
        match self {
            Self::Hello => 0x01,
            Self::Ready => 0x02,
            Self::DataC2m => 0x03,
            Self::AckC2m => 0x04,
            Self::DataM2c => 0x05,
            Self::RateProbe => 0x06,
            Self::RateProbeAck => 0x07,
            Self::Reset => 0x08,
            Self::ResetAck => 0x09,
            Self::Error => 0x0a,
            Self::Ping => 0x0b,
            Self::Pong => 0x0c,
            Self::Unknown(raw) => raw,
        }
    }

    fn from_u8(value: u8) -> Self {
        match value {
            0x01 => Self::Hello,
            0x02 => Self::Ready,
            0x03 => Self::DataC2m,
            0x04 => Self::AckC2m,
            0x05 => Self::DataM2c,
            0x06 => Self::RateProbe,
            0x07 => Self::RateProbeAck,
            0x08 => Self::Reset,
            0x09 => Self::ResetAck,
            0x0a => Self::Error,
            0x0b => Self::Ping,
            0x0c => Self::Pong,
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

pub fn encode(frame_type: FrameType, seq: u16, ack: u16, payload: &[u8]) -> Option<Vec<u8>> {
    if payload.len() > MAX_PAYLOAD {
        return None;
    }
    if matches!(frame_type, FrameType::Unknown(_)) {
        return None;
    }

    let payload_len = u16::try_from(payload.len()).ok()?;
    let mut body = Vec::with_capacity(BODY_HEADER_LEN + payload.len() + CRC_LEN);
    body.push(VERSION);
    body.push(frame_type.to_u8());
    body.push(0);
    body.extend_from_slice(&seq.to_le_bytes());
    body.extend_from_slice(&ack.to_le_bytes());
    body.extend_from_slice(&payload_len.to_le_bytes());
    body.extend_from_slice(payload);
    let crc = crc16(&body);
    body.extend_from_slice(&crc.to_le_bytes());

    let mut encoded = cobs_encode(&body);
    encoded.push(DELIMITER);

    if encoded.len() > MAX_ENCODED_LEN {
        return None;
    }

    Some(encoded)
}

pub fn hello_payload(desired_payload: u16, desired_credit: u16) -> [u8; 6] {
    let desired_payload = desired_payload.to_le_bytes();
    let desired_credit = desired_credit.to_le_bytes();
    let supported_rate_mask = SUPPORTED_RATE_MASK.to_le_bytes();
    [
        desired_payload[0],
        desired_payload[1],
        desired_credit[0],
        desired_credit[1],
        supported_rate_mask[0],
        supported_rate_mask[1],
    ]
}

pub fn parse_ready(payload: &[u8]) -> Option<ReadyPayload> {
    if payload.len() != 11 {
        return None;
    }

    Some(ReadyPayload {
        negotiated_payload: u16::from_le_bytes([payload[0], payload[1]]),
        credit_cap: u16::from_le_bytes([payload[2], payload[3]]),
        initial_credit: u16::from_le_bytes([payload[4], payload[5]]),
        supported_rate_mask: u16::from_le_bytes([payload[6], payload[7]]),
        initial_rate_profile: payload[8],
        flags: u16::from_le_bytes([payload[9], payload[10]]),
    })
}

pub fn ack_credit(payload: &[u8]) -> Option<u16> {
    if payload.len() != 2 {
        return None;
    }

    Some(u16::from_le_bytes([payload[0], payload[1]]))
}

#[derive(Default)]
pub struct Decoder {
    buffer: Vec<u8>,
    discarding: bool,
}

impl Decoder {
    pub fn new() -> Self {
        Self::default()
    }

    pub fn feed(&mut self, data: &[u8]) -> Vec<DecodeEvent> {
        let mut events = Vec::new();

        for byte in data {
            if self.discarding {
                if *byte == DELIMITER {
                    self.discarding = false;
                    events.push(DecodeEvent::Error(DecodeError::Length));
                }
                continue;
            }

            if *byte == DELIMITER {
                if self.buffer.is_empty() {
                    continue;
                }

                let mut encoded = std::mem::take(&mut self.buffer);
                encoded.push(DELIMITER);
                match decode_one(&encoded) {
                    Ok(frame) => events.push(DecodeEvent::Frame(frame)),
                    Err(error) => events.push(DecodeEvent::Error(error)),
                }
                continue;
            }

            self.buffer.push(*byte);
            if self.buffer.len() >= MAX_ENCODED_LEN {
                self.buffer.clear();
                self.discarding = true;
            }
        }

        events
    }
}

fn cobs_encode(input: &[u8]) -> Vec<u8> {
    let mut encoded = Vec::with_capacity(input.len() + 1);
    let mut code_index = 0usize;
    let mut code = 1u8;
    let mut block_open = true;

    encoded.push(0);

    for byte in input {
        if !block_open {
            code_index = encoded.len();
            encoded.push(0);
            code = 1;
            block_open = true;
        }

        if *byte == 0 {
            encoded[code_index] = code;
            block_open = false;
            continue;
        }

        encoded.push(*byte);
        code = code.wrapping_add(1);
        if code == 0xff {
            encoded[code_index] = code;
            block_open = false;
        }
    }

    if block_open {
        encoded[code_index] = code;
    } else if encoded.is_empty() || input.last().copied() == Some(0) {
        encoded.push(1);
    }
    encoded
}

fn cobs_decode(input: &[u8]) -> Result<Vec<u8>, DecodeError> {
    let mut decoded = Vec::with_capacity(input.len());
    let mut read_index = 0usize;

    while read_index < input.len() {
        let code = input[read_index];
        read_index += 1;

        if code == 0 {
            return Err(DecodeError::Cobs);
        }

        let copy_len = usize::from(code - 1);
        if copy_len > input.len().saturating_sub(read_index) {
            return Err(DecodeError::Cobs);
        }

        decoded.extend_from_slice(&input[read_index..read_index + copy_len]);
        read_index += copy_len;

        if code != 0xff && read_index < input.len() {
            decoded.push(0);
        }
    }

    Ok(decoded)
}

fn decode_one(encoded_with_delimiter: &[u8]) -> Result<Frame, DecodeError> {
    if encoded_with_delimiter.len() > MAX_ENCODED_LEN
        || encoded_with_delimiter.last().copied() != Some(DELIMITER)
    {
        return Err(DecodeError::Length);
    }

    let encoded = &encoded_with_delimiter[..encoded_with_delimiter.len() - 1];
    if encoded.is_empty() {
        return Err(DecodeError::Length);
    }
    if encoded.iter().any(|byte| *byte == DELIMITER) {
        return Err(DecodeError::Cobs);
    }

    let body = cobs_decode(encoded)?;
    if body.len() < BODY_HEADER_LEN + CRC_LEN {
        return Err(DecodeError::Length);
    }

    if body[0] != VERSION {
        return Err(DecodeError::Version(body[0]));
    }

    let payload_len = u16::from_le_bytes([body[7], body[8]]) as usize;
    if payload_len > MAX_PAYLOAD {
        return Err(DecodeError::PayloadTooLarge);
    }

    let expected_body_len = BODY_HEADER_LEN + payload_len + CRC_LEN;
    if body.len() != expected_body_len {
        return Err(DecodeError::Length);
    }

    let crc_offset = BODY_HEADER_LEN + payload_len;
    let expected_crc = u16::from_le_bytes([body[crc_offset], body[crc_offset + 1]]);
    let actual_crc = crc16(&body[..crc_offset]);
    if actual_crc != expected_crc {
        return Err(DecodeError::Crc);
    }

    Ok(Frame {
        frame_type: FrameType::from_u8(body[1]),
        flags: body[2],
        seq: u16::from_le_bytes([body[3], body[4]]),
        ack: u16::from_le_bytes([body[5], body[6]]),
        payload: body[BODY_HEADER_LEN..crc_offset].to_vec(),
    })
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn constants_match_firmware_contract() {
        assert_eq!(VERSION, 2);
        assert_eq!(DELIMITER, 0);
        assert_eq!(BODY_HEADER_LEN, 9);
        assert_eq!(CRC_LEN, 2);
        assert_eq!(MAX_ENCODED_LEN, 512);
        assert_eq!(MAX_PAYLOAD, 497);
        assert_eq!(DEFAULT_PAYLOAD, MAX_PAYLOAD);
        assert_eq!(INITIAL_CREDIT, 512);
        assert_eq!(SUPPORTED_RATE_MASK, 0x03ff);
        assert_eq!(STARTUP_RATE_PROFILE, 0);
    }

    #[test]
    fn frame_type_ids_match_v2_contract() {
        assert_eq!(FrameType::Hello.to_u8(), 0x01);
        assert_eq!(FrameType::Ready.to_u8(), 0x02);
        assert_eq!(FrameType::DataC2m.to_u8(), 0x03);
        assert_eq!(FrameType::AckC2m.to_u8(), 0x04);
        assert_eq!(FrameType::DataM2c.to_u8(), 0x05);
        assert_eq!(FrameType::RateProbe.to_u8(), 0x06);
        assert_eq!(FrameType::RateProbeAck.to_u8(), 0x07);
        assert_eq!(FrameType::Reset.to_u8(), 0x08);
        assert_eq!(FrameType::ResetAck.to_u8(), 0x09);
        assert_eq!(FrameType::Error.to_u8(), 0x0a);
        assert_eq!(FrameType::Ping.to_u8(), 0x0b);
        assert_eq!(FrameType::Pong.to_u8(), 0x0c);
    }

    #[test]
    fn hello_payload_uses_v2_capability_shape() {
        assert_eq!(
            hello_payload(0x01f1, 0x0200),
            [0xf1, 0x01, 0x00, 0x02, 0xff, 0x03]
        );
    }

    #[test]
    fn parse_ready_accepts_v2_capability_shape() {
        let ready = parse_ready(&[
            0xf1, 0x01, 0x00, 0x02, 0xf0, 0x01, 0xff, 0x03, 0x00, 0x34, 0x12,
        ])
        .unwrap();

        assert_eq!(ready.negotiated_payload, 0x01f1);
        assert_eq!(ready.credit_cap, 0x0200);
        assert_eq!(ready.initial_credit, 0x01f0);
        assert_eq!(ready.supported_rate_mask, 0x03ff);
        assert_eq!(ready.initial_rate_profile, 0);
        assert_eq!(ready.flags, 0x1234);
    }

    #[test]
    fn parse_ready_rejects_old_5_byte_shape() {
        assert!(parse_ready(&[VERSION, 0xf1, 0x01, 0x00, 0x02]).is_none());
    }

    #[test]
    fn ack_credit_parses_absolute_credit() {
        assert_eq!(ack_credit(&[0x34, 0x12]), Some(0x1234));
        assert_eq!(ack_credit(&[0x34]), None);
    }

    #[test]
    fn v2_golden_vector_matches_c_codec() {
        let payload = [0x00, 0x11, 0x22];
        assert_eq!(
            encode(FrameType::DataC2m, 0x1234, 0x00f0, &payload).unwrap(),
            vec![
                0x03, 0x02, 0x03, 0x04, 0x34, 0x12, 0xf0, 0x02, 0x03, 0x01, 0x05, 0x11, 0x22, 0x3e,
                0xd5, 0x00,
            ]
        );
    }

    #[test]
    fn v2_cobs_frame_uses_zero_delimiter_and_no_inner_zeroes() {
        let payload = [0x00, 0x11, 0x00, 0x22];
        let encoded = encode(FrameType::DataC2m, 0x1234, 0, &payload).unwrap();
        assert_eq!(encoded.last().copied(), Some(0));
        assert!(encoded[..encoded.len() - 1].iter().all(|byte| *byte != 0));
        let mut decoder = Decoder::new();
        let events = decoder.feed(&encoded);
        assert_eq!(
            events,
            vec![DecodeEvent::Frame(Frame {
                frame_type: FrameType::DataC2m,
                flags: 0,
                seq: 0x1234,
                ack: 0,
                payload: payload.to_vec(),
            })]
        );
    }

    #[test]
    fn v2_decoder_reports_crc_error_after_corruption() {
        let mut encoded = encode(FrameType::DataM2c, 3, 0, b"abc").unwrap();
        let last_data = encoded.len() - 2;
        encoded[last_data] ^= 0x20;
        let mut decoder = Decoder::new();
        let events = decoder.feed(&encoded);
        assert!(matches!(
            events.as_slice(),
            [DecodeEvent::Error(DecodeError::Crc)]
        ));
    }

    #[test]
    fn v2_max_payload_fits_and_decodes() {
        let payload = vec![0xa5; MAX_PAYLOAD];
        let encoded = encode(FrameType::DataM2c, 7, 0x2244, &payload).unwrap();
        assert!(encoded.len() <= MAX_ENCODED_LEN);
        assert_eq!(encoded.last().copied(), Some(DELIMITER));
        let mut decoder = Decoder::new();
        let events = decoder.feed(&encoded);
        assert_eq!(
            events,
            vec![DecodeEvent::Frame(Frame {
                frame_type: FrameType::DataM2c,
                flags: 0,
                seq: 7,
                ack: 0x2244,
                payload,
            })]
        );
    }

    #[test]
    fn v2_decode_one_rejects_missing_delimiter() {
        let mut encoded = encode(FrameType::DataC2m, 1, 0, b"x").unwrap();
        encoded.pop();
        assert_eq!(decode_one(&encoded), Err(DecodeError::Length));
    }

    #[test]
    fn v2_decoder_waits_when_delimiter_is_missing() {
        let mut encoded = encode(FrameType::DataC2m, 1, 0, b"x").unwrap();
        encoded.pop();
        let mut decoder = Decoder::new();
        assert!(decoder.feed(&encoded).is_empty());
    }

    #[test]
    fn v2_decode_one_rejects_malformed_cobs() {
        assert_eq!(decode_one(&[0x02, DELIMITER]), Err(DecodeError::Cobs));
    }

    #[test]
    fn v2_cobs_full_span_does_not_add_trailing_empty_block() {
        let encoded = cobs_encode(&[0x7e; 254]);
        assert_eq!(encoded.len(), 255);
        assert_eq!(encoded[0], 0xff);
        assert_eq!(cobs_decode(&encoded), Ok(vec![0x7e; 254]));
    }

    #[test]
    fn v2_cobs_encodes_raw_zero_body() {
        let body = [0];
        let encoded = cobs_encode(&body);
        assert_eq!(encoded, vec![0x01, 0x01]);
        assert_eq!(cobs_decode(&encoded), Ok(body.to_vec()));
    }

    #[test]
    fn v2_cobs_encodes_raw_nonzero_then_zero_body() {
        let body = [1, 0];
        let encoded = cobs_encode(&body);
        assert_eq!(encoded, vec![0x02, 0x01, 0x01]);
        assert_eq!(cobs_decode(&encoded), Ok(body.to_vec()));
    }

    #[test]
    fn v2_cobs_encodes_raw_255_byte_nonzero_span() {
        let body = vec![0x7e; 255];
        let encoded = cobs_encode(&body);
        assert_eq!(encoded.len(), 257);
        assert_eq!(encoded[0], 0xff);
        assert_eq!(encoded[255], 0x02);
        assert_eq!(cobs_decode(&encoded), Ok(body));
    }

    #[test]
    fn v2_cobs_encodes_raw_full_span_plus_trailing_zero() {
        let mut body = vec![0x7e; 254];
        body.push(0);
        let encoded = cobs_encode(&body);
        assert_eq!(encoded.len(), 257);
        assert_eq!(encoded[0], 0xff);
        assert_eq!(encoded[255], 0x01);
        assert_eq!(encoded[256], 0x01);
        assert_eq!(cobs_decode(&encoded), Ok(body));
    }

    #[test]
    fn v2_decoder_does_not_recover_valid_frame_without_delimiter_boundary() {
        let valid = encode(FrameType::Pong, 4, 3, b"ok").unwrap();
        let mut stream = vec![0x55, 0x66];
        stream.extend_from_slice(&valid);
        stream.extend_from_slice(&valid);

        let mut decoder = Decoder::new();
        let events = decoder.feed(&stream);
        assert_eq!(
            events,
            vec![
                DecodeEvent::Error(DecodeError::Cobs),
                DecodeEvent::Frame(Frame {
                    frame_type: FrameType::Pong,
                    flags: 0,
                    seq: 4,
                    ack: 3,
                    payload: b"ok".to_vec(),
                }),
            ]
        );
    }

    #[test]
    fn v2_decode_one_rejects_bad_version() {
        let body = body_with_crc(3, FrameType::Ready, 0, 0, 0, &[]);
        let mut encoded = cobs_encode(&body);
        encoded.push(DELIMITER);
        assert_eq!(decode_one(&encoded), Err(DecodeError::Version(3)));
    }

    #[test]
    fn v2_decode_one_rejects_oversized_payload_length() {
        let body = body_with_crc(
            VERSION,
            FrameType::DataC2m,
            0,
            0,
            0,
            &vec![0x55; MAX_PAYLOAD + 1],
        );
        let mut encoded = cobs_encode(&body);
        encoded.push(DELIMITER);
        assert_eq!(decode_one(&encoded), Err(DecodeError::PayloadTooLarge));
    }

    #[test]
    fn v2_decode_one_rejects_length_mismatch() {
        let mut body = Vec::new();
        body.push(VERSION);
        body.push(FrameType::DataC2m.to_u8());
        body.push(0);
        body.extend_from_slice(&1u16.to_le_bytes());
        body.extend_from_slice(&0u16.to_le_bytes());
        body.extend_from_slice(&2u16.to_le_bytes());
        body.push(0x99);
        let crc = crc16(&body);
        body.extend_from_slice(&crc.to_le_bytes());

        let mut encoded = cobs_encode(&body);
        encoded.push(DELIMITER);
        assert_eq!(decode_one(&encoded), Err(DecodeError::Length));
    }

    #[test]
    fn v2_decoder_accepts_split_frame() {
        let encoded = encode(FrameType::DataC2m, 0x22, 0x11, b"a\0c").unwrap();
        let mut decoder = Decoder::new();
        assert!(decoder.feed(&encoded[..2]).is_empty());
        let events = decoder.feed(&encoded[2..]);
        assert_eq!(
            events,
            vec![DecodeEvent::Frame(Frame {
                frame_type: FrameType::DataC2m,
                flags: 0,
                seq: 0x22,
                ack: 0x11,
                payload: b"a\0c".to_vec(),
            })]
        );
    }

    #[test]
    fn v2_decoder_returns_multiple_frames_from_one_feed() {
        let first = encode(FrameType::Ping, 1, 0, b"one").unwrap();
        let second = encode(FrameType::Pong, 2, 1, b"two").unwrap();
        let mut stream = first;
        stream.extend_from_slice(&second);

        let mut decoder = Decoder::new();
        let events = decoder.feed(&stream);
        assert_eq!(
            events,
            vec![
                DecodeEvent::Frame(Frame {
                    frame_type: FrameType::Ping,
                    flags: 0,
                    seq: 1,
                    ack: 0,
                    payload: b"one".to_vec(),
                }),
                DecodeEvent::Frame(Frame {
                    frame_type: FrameType::Pong,
                    flags: 0,
                    seq: 2,
                    ack: 1,
                    payload: b"two".to_vec(),
                }),
            ]
        );
    }

    #[test]
    fn v2_decoder_ignores_empty_delimiters() {
        let encoded = encode(FrameType::Ping, 0, 0, &[]).unwrap();
        let mut stream = vec![DELIMITER, DELIMITER];
        stream.extend_from_slice(&encoded);

        let mut decoder = Decoder::new();
        let events = decoder.feed(&stream);
        assert_eq!(
            events,
            vec![DecodeEvent::Frame(Frame {
                frame_type: FrameType::Ping,
                flags: 0,
                seq: 0,
                ack: 0,
                payload: Vec::new(),
            })]
        );
    }

    #[test]
    fn v2_decoder_reports_error_then_resumes_after_next_frame() {
        let bad = [0x02, DELIMITER];
        let good = encode(FrameType::Ready, 0, 0, b"ok").unwrap();
        let mut stream = bad.to_vec();
        stream.extend_from_slice(&good);

        let mut decoder = Decoder::new();
        let events = decoder.feed(&stream);
        assert_eq!(
            events,
            vec![
                DecodeEvent::Error(DecodeError::Cobs),
                DecodeEvent::Frame(Frame {
                    frame_type: FrameType::Ready,
                    flags: 0,
                    seq: 0,
                    ack: 0,
                    payload: b"ok".to_vec(),
                }),
            ]
        );
    }

    #[test]
    fn v2_decoder_reports_overflow_once_and_discards_until_delimiter() {
        let poisoned = encode(FrameType::Ready, 1, 0, &[]).unwrap();
        let valid = encode(FrameType::Pong, 2, 1, &[]).unwrap();
        let mut decoder = Decoder::new();

        assert!(decoder.feed(&vec![0x55; MAX_ENCODED_LEN]).is_empty());

        let mut overflow = vec![0x55];
        overflow.extend_from_slice(&poisoned);
        assert_eq!(
            decoder.feed(&overflow),
            vec![DecodeEvent::Error(DecodeError::Length)]
        );

        let events = decoder.feed(&valid);
        assert_eq!(
            events,
            vec![DecodeEvent::Frame(Frame {
                frame_type: FrameType::Pong,
                flags: 0,
                seq: 2,
                ack: 1,
                payload: Vec::new(),
            })]
        );
    }

    #[test]
    fn v2_encode_rejects_payload_over_cap() {
        assert!(encode(FrameType::DataC2m, 0, 0, &vec![0; MAX_PAYLOAD + 1]).is_none());
    }

    #[test]
    fn v2_encode_rejects_unknown_frame_type() {
        assert!(encode(FrameType::Unknown(0xfe), 0, 0, b"unexpected").is_none());
    }

    fn body_with_crc(
        version: u8,
        frame_type: FrameType,
        flags: u8,
        seq: u16,
        ack: u16,
        payload: &[u8],
    ) -> Vec<u8> {
        let mut body = Vec::new();
        body.push(version);
        body.push(frame_type.to_u8());
        body.push(flags);
        body.extend_from_slice(&seq.to_le_bytes());
        body.extend_from_slice(&ack.to_le_bytes());
        body.extend_from_slice(&(payload.len() as u16).to_le_bytes());
        body.extend_from_slice(payload);
        let crc = crc16(&body);
        body.extend_from_slice(&crc.to_le_bytes());
        body
    }
}
