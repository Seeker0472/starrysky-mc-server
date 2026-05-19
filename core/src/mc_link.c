#include "mc_link.h"

#include <string.h>

#define MC_LINK_CRC_INIT 0xffffu
#define MC_LINK_CRC_POLY 0x1021u
#define MC_LINK_MAX_BODY_LEN (MC_LINK_BODY_HEADER_LEN + MC_LINK_MAX_PAYLOAD + MC_LINK_CRC_LEN)
#define MC_LINK_MAX_ENCODED_WITHOUT_DELIMITER (MC_LINK_MAX_ENCODED_LEN - 1u)

typedef struct {
    uint8_t *dst;
    size_t dst_cap;
    size_t write_index;
    size_t code_index;
    uint8_t code;
    uint8_t block_open;
    uint8_t trailing_zero_pending;
} cobs_stream_encoder_t;

static uint16_t crc16_update(uint16_t crc, const uint8_t *src, size_t len)
{
    if (src == NULL && len > 0u) {
        return crc;
    }

    for (size_t i = 0; i < len; ++i) {
        crc ^= (uint16_t)src[i] << 8;
        for (unsigned int bit = 0; bit < 8u; ++bit) {
            if ((crc & 0x8000u) != 0u) {
                crc = (uint16_t)((crc << 1) ^ MC_LINK_CRC_POLY);
            } else {
                crc = (uint16_t)(crc << 1);
            }
        }
    }

    return crc;
}

static int bytes_contain(uint8_t needle, const uint8_t *src, size_t len)
{
    if (src == NULL && len > 0u) {
        return 0;
    }

    for (size_t i = 0u; i < len; ++i) {
        if (src[i] == needle) {
            return 1;
        }
    }
    return 0;
}

uint16_t mc_link_crc16(const uint8_t *src, size_t len)
{
    return crc16_update(MC_LINK_CRC_INIT, src, len);
}

static int cobs_stream_open_block(cobs_stream_encoder_t *encoder)
{
    if (encoder->write_index >= encoder->dst_cap) {
        return 0;
    }

    encoder->code_index = encoder->write_index++;
    encoder->code = 1u;
    encoder->block_open = 1u;
    encoder->trailing_zero_pending = 0u;
    return 1;
}

static int cobs_stream_init(cobs_stream_encoder_t *encoder, uint8_t *dst, size_t dst_cap)
{
    if (encoder == NULL || dst == NULL || dst_cap == 0u) {
        return 0;
    }

    encoder->dst = dst;
    encoder->dst_cap = dst_cap;
    encoder->write_index = 0u;
    encoder->code_index = 0u;
    encoder->code = 1u;
    encoder->block_open = 0u;
    encoder->trailing_zero_pending = 0u;
    return 1;
}

static int cobs_stream_write_byte(cobs_stream_encoder_t *encoder, uint8_t byte)
{
    if (!encoder->block_open && !cobs_stream_open_block(encoder)) {
        return 0;
    }

    if (byte == 0u) {
        encoder->dst[encoder->code_index] = encoder->code;
        encoder->block_open = 0u;
        encoder->trailing_zero_pending = 1u;
        return 1;
    }

    if (encoder->write_index >= encoder->dst_cap) {
        return 0;
    }
    encoder->dst[encoder->write_index++] = byte;
    encoder->code++;
    if (encoder->code == 0xffu) {
        encoder->dst[encoder->code_index] = encoder->code;
        encoder->block_open = 0u;
        encoder->trailing_zero_pending = 0u;
    }
    return 1;
}

static int cobs_stream_write(cobs_stream_encoder_t *encoder, const uint8_t *src, size_t src_len)
{
    if (src == NULL && src_len > 0u) {
        return 0;
    }

    for (size_t i = 0u; i < src_len; ++i) {
        if (!cobs_stream_write_byte(encoder, src[i])) {
            return 0;
        }
    }
    return 1;
}

static int cobs_stream_finish(cobs_stream_encoder_t *encoder, size_t *dst_len)
{
    if (encoder == NULL || dst_len == NULL) {
        return 0;
    }

    if (encoder->block_open) {
        encoder->dst[encoder->code_index] = encoder->code;
    } else if (encoder->write_index == 0u || encoder->trailing_zero_pending) {
        if (!cobs_stream_open_block(encoder)) {
            return 0;
        }
        encoder->dst[encoder->code_index] = encoder->code;
    }

    *dst_len = encoder->write_index;
    return 1;
}

static int cobs_decode(const uint8_t *src, size_t src_len, uint8_t *dst, size_t dst_cap, size_t *dst_len)
{
    size_t read_index = 0u;
    size_t write_index = 0u;

    if (dst_len == NULL || dst == NULL || (src == NULL && src_len > 0u)) {
        return 0;
    }

    *dst_len = 0u;
    while (read_index < src_len) {
        uint8_t code = src[read_index++];
        uint8_t copy_len;

        if (code == 0u) {
            return 0;
        }

        copy_len = (uint8_t)(code - 1u);
        if ((size_t)copy_len > src_len - read_index) {
            return 0;
        }
        if (write_index + copy_len > dst_cap) {
            return 0;
        }
        if (copy_len > 0u) {
            memcpy(dst + write_index, src + read_index, copy_len);
            write_index += copy_len;
            read_index += copy_len;
        }

        if (code != 0xffu && read_index < src_len) {
            if (write_index == dst_cap) {
                return 0;
            }
            dst[write_index++] = 0u;
        }
    }

    *dst_len = write_index;
    return 1;
}

int mc_link_encode(uint8_t type,
                   uint16_t seq,
                   uint16_t ack,
                   const uint8_t *payload,
                   size_t payload_len,
                   uint8_t *out,
                   size_t out_cap,
                   size_t *out_len)
{
    uint8_t header[MC_LINK_BODY_HEADER_LEN];
    uint8_t crc_bytes[MC_LINK_CRC_LEN];
    cobs_stream_encoder_t encoder;
    size_t encoded_len;
    uint16_t crc;

    if (out_len != NULL) {
        *out_len = 0u;
    }

    if (out == NULL || out_len == NULL) {
        return 0;
    }
    if (payload_len > MC_LINK_MAX_PAYLOAD) {
        return 0;
    }
    if (payload_len > 0u && payload == NULL) {
        return 0;
    }

    if (out_cap == 0u) {
        return 0;
    }

    header[0] = MC_LINK_VERSION;
    header[1] = type;
    header[2] = 0u;
    header[3] = (uint8_t)(seq & 0xffu);
    header[4] = (uint8_t)((seq >> 8) & 0xffu);
    header[5] = (uint8_t)(ack & 0xffu);
    header[6] = (uint8_t)((ack >> 8) & 0xffu);
    header[7] = (uint8_t)(payload_len & 0xffu);
    header[8] = (uint8_t)((payload_len >> 8) & 0xffu);

    crc = crc16_update(MC_LINK_CRC_INIT, header, sizeof(header));
    if (payload_len > 0u) {
        crc = crc16_update(crc, payload, payload_len);
    }
    crc_bytes[0] = (uint8_t)(crc & 0xffu);
    crc_bytes[1] = (uint8_t)((crc >> 8) & 0xffu);

    if (!cobs_stream_init(&encoder, out, out_cap - 1u) ||
        !cobs_stream_write(&encoder, header, sizeof(header)) ||
        !cobs_stream_write(&encoder, payload, payload_len) ||
        !cobs_stream_write(&encoder, crc_bytes, sizeof(crc_bytes)) ||
        !cobs_stream_finish(&encoder, &encoded_len)) {
        return 0;
    }
    if (encoded_len > MC_LINK_MAX_ENCODED_WITHOUT_DELIMITER || encoded_len + 1u > out_cap) {
        return 0;
    }

    out[encoded_len] = MC_LINK_DELIMITER;
    *out_len = encoded_len + 1u;
    return 1;
}

static int decode_frame_with_scratch(const uint8_t *src,
                                     size_t src_len,
                                     mc_link_frame_t *frame,
                                     uint8_t *body,
                                     size_t body_cap)
{
    size_t encoded_len;
    size_t body_len;
    size_t payload_len;
    uint16_t expected_crc;
    uint16_t actual_crc;

    if (frame == NULL || body == NULL || src == NULL || src_len == 0u) {
        return 0;
    }
    if (src_len > MC_LINK_MAX_ENCODED_LEN || src[src_len - 1u] != MC_LINK_DELIMITER) {
        return 0;
    }

    encoded_len = src_len - 1u;
    if (encoded_len == 0u || encoded_len > MC_LINK_MAX_ENCODED_WITHOUT_DELIMITER) {
        return 0;
    }
    for (size_t i = 0u; i < encoded_len; ++i) {
        if (src[i] == MC_LINK_DELIMITER) {
            return 0;
        }
    }

    if (!cobs_decode(src, encoded_len, body, body_cap, &body_len)) {
        return 0;
    }
    if (body_len < MC_LINK_BODY_HEADER_LEN + MC_LINK_CRC_LEN) {
        return 0;
    }
    if (body[0] != MC_LINK_VERSION) {
        return 0;
    }

    payload_len = (size_t)body[7] | ((size_t)body[8] << 8);
    if (payload_len > MC_LINK_MAX_PAYLOAD) {
        return 0;
    }
    if (body_len != MC_LINK_BODY_HEADER_LEN + payload_len + MC_LINK_CRC_LEN) {
        return 0;
    }

    expected_crc = (uint16_t)body[MC_LINK_BODY_HEADER_LEN + payload_len] |
                   ((uint16_t)body[MC_LINK_BODY_HEADER_LEN + payload_len + 1u] << 8);
    actual_crc = mc_link_crc16(body, MC_LINK_BODY_HEADER_LEN + payload_len);
    if (actual_crc != expected_crc) {
        return 0;
    }

    frame->type = body[1];
    frame->flags = body[2];
    frame->seq = (uint16_t)body[3] | ((uint16_t)body[4] << 8);
    frame->ack = (uint16_t)body[5] | ((uint16_t)body[6] << 8);
    frame->len = (uint16_t)payload_len;
    if (payload_len > 0u) {
        memcpy(frame->payload, body + MC_LINK_BODY_HEADER_LEN, payload_len);
    }
    return 1;
}

int mc_link_decode_frame(const uint8_t *src,
                         size_t src_len,
                         mc_link_frame_t *frame)
{
    uint8_t body[MC_LINK_MAX_BODY_LEN];

    return decode_frame_with_scratch(src, src_len, frame, body, sizeof(body));
}

void mc_link_parser_init(mc_link_parser_t *parser)
{
    if (parser == NULL) {
        return;
    }
    memset(parser, 0, sizeof(*parser));
}

static void parser_drop(mc_link_parser_t *parser, size_t count)
{
    if (count >= parser->len) {
        parser->len = 0u;
        return;
    }

    memmove(parser->buf, parser->buf + count, parser->len - count);
    parser->len -= count;
}

static mc_link_parse_result_t parser_try_one(mc_link_parser_t *parser,
                                             mc_link_frame_t *frame)
{
    for (size_t i = 0u; i < parser->len; ++i) {
        if (parser->buf[i] == MC_LINK_DELIMITER) {
            size_t frame_len = i + 1u;

            if (decode_frame_with_scratch(parser->buf,
                                          frame_len,
                                          frame,
                                          parser->decode_scratch,
                                          sizeof(parser->decode_scratch))) {
                parser_drop(parser, frame_len);
                return MC_LINK_PARSE_FRAME;
            }

            parser->resync_count++;
            if (i == 0u || frame_len > MC_LINK_MAX_FRAME_LEN) {
                parser->length_error_count++;
            } else {
                parser->crc_error_count++;
            }
            parser_drop(parser, frame_len);
            return MC_LINK_PARSE_NEED_MORE;
        }
    }

    if (parser->len == MC_LINK_MAX_FRAME_LEN) {
        parser->length_error_count++;
        parser->resync_count++;
        parser->len = 0u;
        parser->discarding = 1u;
    }

    return MC_LINK_PARSE_NEED_MORE;
}

mc_link_parse_result_t mc_link_parser_feed(mc_link_parser_t *parser,
                                           const uint8_t *src,
                                           size_t len,
                                           mc_link_frame_t *frame)
{
    if (parser == NULL || frame == NULL || (src == NULL && len > 0u)) {
        return MC_LINK_PARSE_ERROR;
    }

    if (parser->discarding) {
        parser->len = 0u;
    } else {
        for (;;) {
            size_t before_len = parser->len;
            mc_link_parse_result_t result = parser_try_one(parser, frame);
            if (result == MC_LINK_PARSE_FRAME) {
                return result;
            }
            if (parser->len == before_len) {
                break;
            }
        }
    }

    for (size_t i = 0u; i < len; ++i) {
        if (parser->discarding) {
            if (src[i] == MC_LINK_DELIMITER) {
                parser->discarding = 0u;
            }
            continue;
        }

        parser->buf[parser->len++] = src[i];
        if (src[i] == MC_LINK_DELIMITER) {
            mc_link_parse_result_t result = parser_try_one(parser, frame);
            if (result == MC_LINK_PARSE_FRAME) {
                if (i + 1u < len) {
                    size_t remaining = len - i - 1u;
                    size_t capacity = sizeof(parser->buf) - parser->len;
                    if (remaining > capacity) {
                        parser->length_error_count++;
                        parser->resync_count++;
                        parser->len = 0u;
                        parser->discarding = 1u;
                    } else {
                        memcpy(parser->buf + parser->len, src + i + 1u, remaining);
                        parser->len += remaining;
                        if (remaining == capacity &&
                            !bytes_contain(MC_LINK_DELIMITER, src + i + 1u, remaining)) {
                            parser->length_error_count++;
                            parser->resync_count++;
                            parser->len = 0u;
                            parser->discarding = 1u;
                        }
                    }
                }
                return result;
            }
        } else if (parser->len == MC_LINK_MAX_FRAME_LEN) {
            parser->length_error_count++;
            parser->resync_count++;
            parser->len = 0u;
            parser->discarding = 1u;
        }
    }

    return parser_try_one(parser, frame);
}
