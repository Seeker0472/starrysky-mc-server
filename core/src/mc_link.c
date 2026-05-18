#include "mc_link.h"

#include <string.h>

#define MC_LINK_CRC_INIT 0xffffu
#define MC_LINK_CRC_POLY 0x1021u

uint16_t mc_link_crc16(const uint8_t *src, size_t len)
{
    uint16_t crc = MC_LINK_CRC_INIT;

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

int mc_link_encode(uint8_t type,
                   uint8_t seq,
                   const uint8_t *payload,
                   size_t payload_len,
                   uint8_t *out,
                   size_t out_cap,
                   size_t *out_len)
{
    size_t frame_len;
    uint16_t crc;

    if (out_len != NULL) {
        *out_len = 0u;
    }

    if (out == NULL || out_len == NULL) {
        return 0;
    }
    if (payload_len > MC_LINK_FIRMWARE_PAYLOAD_CAP) {
        return 0;
    }
    if (payload_len > 0u && payload == NULL) {
        return 0;
    }
    frame_len = MC_LINK_HEADER_LEN + payload_len + MC_LINK_CRC_LEN;
    if (out_cap < frame_len) {
        return 0;
    }

    out[0] = MC_LINK_MAGIC0;
    out[1] = MC_LINK_MAGIC1;
    out[2] = type;
    out[3] = seq;
    out[4] = (uint8_t)(payload_len & 0xffu);
    out[5] = (uint8_t)((payload_len >> 8) & 0xffu);
    if (payload_len > 0u) {
        memcpy(out + MC_LINK_HEADER_LEN, payload, payload_len);
    }

    crc = mc_link_crc16(out + 2u, 4u + payload_len);
    out[MC_LINK_HEADER_LEN + payload_len] = (uint8_t)(crc & 0xffu);
    out[MC_LINK_HEADER_LEN + payload_len + 1u] = (uint8_t)((crc >> 8) & 0xffu);
    *out_len = frame_len;
    return 1;
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

static void parser_resync(mc_link_parser_t *parser)
{
    size_t keep = 0u;

    for (size_t i = 1u; i + 1u < parser->len; ++i) {
        if (parser->buf[i] == MC_LINK_MAGIC0 && parser->buf[i + 1u] == MC_LINK_MAGIC1) {
            keep = i;
            break;
        }
    }

    if (keep != 0u) {
        parser_drop(parser, keep);
        parser->resync_count++;
        return;
    }

    if (parser->len > 0u && parser->buf[parser->len - 1u] == MC_LINK_MAGIC0) {
        parser->buf[0] = MC_LINK_MAGIC0;
        parser->len = 1u;
    } else {
        parser->len = 0u;
    }
    parser->resync_count++;
}

static mc_link_parse_result_t parser_try_one(mc_link_parser_t *parser,
                                             mc_link_frame_t *frame)
{
    size_t payload_len;
    size_t frame_len;
    uint16_t expected_crc;
    uint16_t actual_crc;

    if (parser->len < 2u) {
        return MC_LINK_PARSE_NEED_MORE;
    }

    if (parser->buf[0] != MC_LINK_MAGIC0 || parser->buf[1] != MC_LINK_MAGIC1) {
        parser_resync(parser);
        return MC_LINK_PARSE_NEED_MORE;
    }

    if (parser->len < MC_LINK_HEADER_LEN) {
        return MC_LINK_PARSE_NEED_MORE;
    }

    payload_len = (size_t)parser->buf[4] | ((size_t)parser->buf[5] << 8);
    if (payload_len > MC_LINK_FIRMWARE_PAYLOAD_CAP) {
        parser->length_error_count++;
        parser_drop(parser, 1u);
        parser_resync(parser);
        return MC_LINK_PARSE_NEED_MORE;
    }

    frame_len = MC_LINK_HEADER_LEN + payload_len + MC_LINK_CRC_LEN;
    if (parser->len < frame_len) {
        return MC_LINK_PARSE_NEED_MORE;
    }

    expected_crc = (uint16_t)parser->buf[MC_LINK_HEADER_LEN + payload_len] |
                   ((uint16_t)parser->buf[MC_LINK_HEADER_LEN + payload_len + 1u] << 8);
    actual_crc = mc_link_crc16(parser->buf + 2u, 4u + payload_len);
    if (actual_crc != expected_crc) {
        parser->crc_error_count++;
        parser_drop(parser, 1u);
        parser_resync(parser);
        return MC_LINK_PARSE_NEED_MORE;
    }

    frame->type = parser->buf[2];
    frame->seq = parser->buf[3];
    frame->len = (uint16_t)payload_len;
    if (payload_len > 0u) {
        memcpy(frame->payload, parser->buf + MC_LINK_HEADER_LEN, payload_len);
    }
    parser_drop(parser, frame_len);
    return MC_LINK_PARSE_FRAME;
}

mc_link_parse_result_t mc_link_parser_feed(mc_link_parser_t *parser,
                                           const uint8_t *src,
                                           size_t len,
                                           mc_link_frame_t *frame)
{
    int have_frame = 0;
    mc_link_frame_t first_frame;

    if (parser == NULL || frame == NULL || (src == NULL && len > 0u)) {
        return MC_LINK_PARSE_ERROR;
    }

    for (;;) {
        size_t before_len = parser->len;
        mc_link_parse_result_t result = parser_try_one(parser, frame);
        if (result == MC_LINK_PARSE_FRAME) {
            return result;
        }
        if (result == MC_LINK_PARSE_ERROR) {
            return result;
        }
        if (parser->len == before_len) {
            break;
        }
    }

    for (size_t i = 0u; i < len; ++i) {
        if (parser->len == MC_LINK_MAX_FRAME_LEN) {
            if (have_frame) {
                break;
            }
            size_t before_len = parser->len;
            mc_link_parse_result_t result = parser_try_one(parser, &first_frame);
            if (result == MC_LINK_PARSE_FRAME) {
                have_frame = 1;
                break;
            } else if (result == MC_LINK_PARSE_ERROR) {
                return result;
            } else if (parser->len == before_len) {
                parser->length_error_count++;
                return MC_LINK_PARSE_ERROR;
            }
        }

        parser->buf[parser->len++] = src[i];

        if (!have_frame) {
            for (;;) {
                size_t before_len = parser->len;
                mc_link_parse_result_t result = parser_try_one(parser, &first_frame);
                if (result == MC_LINK_PARSE_FRAME) {
                    have_frame = 1;
                    break;
                }
                if (result == MC_LINK_PARSE_ERROR) {
                    return result;
                }
                if (parser->len == before_len) {
                    break;
                }
            }
        }
    }

    if (have_frame) {
        *frame = first_frame;
        return MC_LINK_PARSE_FRAME;
    }

    for (;;) {
        size_t before_len = parser->len;
        mc_link_parse_result_t result = parser_try_one(parser, frame);
        if (result != MC_LINK_PARSE_NEED_MORE) {
            return result;
        }
        if (parser->len == before_len) {
            return MC_LINK_PARSE_NEED_MORE;
        }
    }
}
