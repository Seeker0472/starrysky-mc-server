#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include "mc_link.h"

#define ASSERT_TRUE(expr) do { if (!(expr)) return 1; } while (0)
#define ASSERT_EQ(a, b) do { if ((a) != (b)) return 1; } while (0)
#define ASSERT_MEMEQ(a, b, n) do { if (memcmp((a), (b), (n)) != 0) return 1; } while (0)

static size_t test_cobs_wrap(const uint8_t *src, size_t src_len, uint8_t *dst, size_t dst_cap)
{
    size_t read_index = 0u;
    size_t write_index = 1u;
    size_t code_index = 0u;
    uint8_t code = 1u;

    if (dst_cap == 0u) {
        return 0u;
    }

    while (read_index < src_len) {
        if (src[read_index] == 0u) {
            dst[code_index] = code;
            code = 1u;
            if (write_index >= dst_cap) {
                return 0u;
            }
            code_index = write_index++;
            read_index++;
        } else {
            if (write_index >= dst_cap) {
                return 0u;
            }
            dst[write_index++] = src[read_index++];
            code++;
            if (code == 0xffu) {
                dst[code_index] = code;
                if (read_index < src_len) {
                    code = 1u;
                    if (write_index >= dst_cap) {
                        return 0u;
                    }
                    code_index = write_index++;
                }
            }
        }
    }

    dst[code_index] = code;
    if (write_index >= dst_cap) {
        return 0u;
    }
    dst[write_index++] = MC_LINK_DELIMITER;
    return write_index;
}

static void test_write_crc(uint8_t *body, size_t body_len_without_crc)
{
    uint16_t crc = mc_link_crc16(body, body_len_without_crc);
    body[body_len_without_crc] = (uint8_t)(crc & 0xffu);
    body[body_len_without_crc + 1u] = (uint8_t)((crc >> 8) & 0xffu);
}

static int test_v2_golden_vector_small_frame(void)
{
    uint8_t payload[] = {0x00u, 0x11u, 0x22u};
    uint8_t encoded[MC_LINK_MAX_FRAME_LEN];
    size_t encoded_len = 0u;
    mc_link_frame_t frame;
    static const uint8_t expected_body[] = {
        0x02u, 0x03u, 0x00u, 0x34u, 0x12u, 0xf0u, 0x00u, 0x03u,
        0x00u, 0x00u, 0x11u, 0x22u, 0x3eu, 0xd5u
    };
    static const uint8_t expected_encoded[] = {
        0x03u, 0x02u, 0x03u, 0x04u, 0x34u, 0x12u, 0xf0u, 0x02u,
        0x03u, 0x01u, 0x05u, 0x11u, 0x22u, 0x3eu, 0xd5u, 0x00u
    };

    ASSERT_EQ(mc_link_crc16(expected_body, MC_LINK_BODY_HEADER_LEN + sizeof(payload)), 0xd53eu);
    ASSERT_TRUE(mc_link_encode(MC_LINK_DATA_C2M, 0x1234u, 0x00f0u, payload, sizeof(payload),
                               encoded, sizeof(encoded), &encoded_len));
    ASSERT_EQ(encoded_len, sizeof(expected_encoded));
    ASSERT_MEMEQ(encoded, expected_encoded, sizeof(expected_encoded));
    ASSERT_TRUE(mc_link_decode_frame(expected_encoded, sizeof(expected_encoded), &frame));
    ASSERT_EQ(frame.type, MC_LINK_DATA_C2M);
    ASSERT_EQ(frame.flags, 0u);
    ASSERT_EQ(frame.seq, 0x1234u);
    ASSERT_EQ(frame.ack, 0x00f0u);
    ASSERT_EQ(frame.len, sizeof(payload));
    ASSERT_MEMEQ(frame.payload, payload, sizeof(payload));
    return 0;
}

static int test_v2_cobs_round_trip_with_zero_payload_bytes(void)
{
    uint8_t payload[] = {0x00u, 0x11u, 0x00u, 0x22u};
    uint8_t encoded[MC_LINK_MAX_FRAME_LEN];
    size_t encoded_len = 0;
    mc_link_frame_t frame;
    ASSERT_TRUE(mc_link_encode(MC_LINK_DATA_C2M, 0x1234u, 0u, payload, sizeof(payload),
                               encoded, sizeof(encoded), &encoded_len));
    ASSERT_TRUE(encoded_len > 0u);
    ASSERT_EQ(encoded[encoded_len - 1u], 0x00u);
    for (size_t i = 0u; i + 1u < encoded_len; ++i) {
        ASSERT_TRUE(encoded[i] != 0x00u);
    }
    ASSERT_TRUE(mc_link_decode_frame(encoded, encoded_len, &frame));
    ASSERT_EQ(frame.type, MC_LINK_DATA_C2M);
    ASSERT_EQ(frame.seq, 0x1234u);
    ASSERT_EQ(frame.ack, 0u);
    ASSERT_EQ(frame.len, sizeof(payload));
    ASSERT_MEMEQ(frame.payload, payload, sizeof(payload));
    return 0;
}

static int test_v2_decode_rejects_bad_crc(void)
{
    uint8_t payload[] = {'b', 'a', 'd'};
    uint8_t encoded[MC_LINK_MAX_FRAME_LEN];
    size_t encoded_len = 0;
    mc_link_frame_t frame;
    ASSERT_TRUE(mc_link_encode(MC_LINK_DATA_C2M, 1u, 0u, payload, sizeof(payload),
                               encoded, sizeof(encoded), &encoded_len));
    encoded[encoded_len - 3u] ^= 0x40u;
    ASSERT_TRUE(!mc_link_decode_frame(encoded, encoded_len, &frame));
    return 0;
}

static int test_v2_max_payload_fits_512_encoded_bytes(void)
{
    uint8_t payload[MC_LINK_MAX_PAYLOAD];
    uint8_t encoded[MC_LINK_MAX_FRAME_LEN];
    size_t encoded_len = 0;
    mc_link_frame_t frame;
    memset(payload, 0xa5, sizeof(payload));
    ASSERT_TRUE(mc_link_encode(MC_LINK_DATA_M2C, 7u, 0x2244u, payload, sizeof(payload),
                               encoded, sizeof(encoded), &encoded_len));
    ASSERT_TRUE(encoded_len <= MC_LINK_MAX_ENCODED_LEN);
    ASSERT_EQ(encoded[encoded_len - 1u], MC_LINK_DELIMITER);
    ASSERT_TRUE(mc_link_decode_frame(encoded, encoded_len, &frame));
    ASSERT_EQ(frame.type, MC_LINK_DATA_M2C);
    ASSERT_EQ(frame.seq, 7u);
    ASSERT_EQ(frame.ack, 0x2244u);
    ASSERT_EQ(frame.len, sizeof(payload));
    ASSERT_MEMEQ(frame.payload, payload, sizeof(payload));
    return 0;
}

static int test_v2_encode_rejects_payload_over_cap(void)
{
    uint8_t payload[MC_LINK_MAX_PAYLOAD + 1u];
    uint8_t encoded[MC_LINK_MAX_FRAME_LEN + 8u];
    size_t encoded_len = 0;
    memset(payload, 0x5au, sizeof(payload));
    ASSERT_TRUE(!mc_link_encode(MC_LINK_DATA_C2M, 0u, 0u, payload, sizeof(payload),
                                encoded, sizeof(encoded), &encoded_len));
    ASSERT_EQ(encoded_len, 0u);
    return 0;
}

static int test_v2_empty_payload_round_trip(void)
{
    uint8_t encoded[MC_LINK_MAX_FRAME_LEN];
    size_t encoded_len = 0;
    mc_link_frame_t frame;
    ASSERT_TRUE(mc_link_encode(MC_LINK_PING, 0xffffu, 0x4321u, NULL, 0u,
                               encoded, sizeof(encoded), &encoded_len));
    ASSERT_TRUE(mc_link_decode_frame(encoded, encoded_len, &frame));
    ASSERT_EQ(frame.type, MC_LINK_PING);
    ASSERT_EQ(frame.seq, 0xffffu);
    ASSERT_EQ(frame.ack, 0x4321u);
    ASSERT_EQ(frame.len, 0u);
    return 0;
}

static int test_v2_decode_rejects_missing_delimiter(void)
{
    uint8_t payload[] = {0x01u};
    uint8_t encoded[MC_LINK_MAX_FRAME_LEN];
    size_t encoded_len = 0;
    mc_link_frame_t frame;
    ASSERT_TRUE(mc_link_encode(MC_LINK_DATA_C2M, 1u, 0u, payload, sizeof(payload),
                               encoded, sizeof(encoded), &encoded_len));
    ASSERT_TRUE(!mc_link_decode_frame(encoded, encoded_len - 1u, &frame));
    return 0;
}

static int test_v2_decode_rejects_bad_version(void)
{
    uint8_t encoded[MC_LINK_MAX_FRAME_LEN];
    size_t encoded_len = 0;
    mc_link_frame_t frame;
    ASSERT_TRUE(mc_link_encode(MC_LINK_READY, 0u, 0u, NULL, 0u,
                               encoded, sizeof(encoded), &encoded_len));
    encoded[1] ^= 0x01u;
    ASSERT_TRUE(!mc_link_decode_frame(encoded, encoded_len, &frame));
    return 0;
}

static int test_v2_cobs_round_trip_254_byte_span(void)
{
    uint8_t payload[252u];
    uint8_t encoded[MC_LINK_MAX_FRAME_LEN];
    size_t encoded_len = 0;
    mc_link_frame_t frame;
    int saw_full_span = 0;
    memset(payload, 0x7eu, sizeof(payload));
    ASSERT_TRUE(mc_link_encode(MC_LINK_DATA_C2M, 1u, 0u, payload, sizeof(payload),
                               encoded, sizeof(encoded), &encoded_len));
    for (size_t i = 0u; i + 1u < encoded_len; ++i) {
        if (encoded[i] == 0xffu) {
            saw_full_span = 1;
        }
    }
    ASSERT_TRUE(saw_full_span);
    ASSERT_TRUE(mc_link_decode_frame(encoded, encoded_len, &frame));
    ASSERT_EQ(frame.type, MC_LINK_DATA_C2M);
    ASSERT_EQ(frame.len, sizeof(payload));
    ASSERT_MEMEQ(frame.payload, payload, sizeof(payload));
    return 0;
}

static int test_v2_decode_rejects_malformed_cobs(void)
{
    uint8_t malformed[] = {0x02u, MC_LINK_DELIMITER};
    mc_link_frame_t frame;
    ASSERT_TRUE(!mc_link_decode_frame(malformed, sizeof(malformed), &frame));
    return 0;
}

static int test_v2_decode_rejects_decoded_payload_len_over_cap(void)
{
    uint8_t body[MC_LINK_BODY_HEADER_LEN + MC_LINK_CRC_LEN];
    uint8_t encoded[32u];
    size_t encoded_len;
    mc_link_frame_t frame;

    memset(body, 0, sizeof(body));
    body[0] = MC_LINK_VERSION;
    body[1] = MC_LINK_DATA_C2M;
    body[7] = (uint8_t)((MC_LINK_MAX_PAYLOAD + 1u) & 0xffu);
    body[8] = (uint8_t)(((MC_LINK_MAX_PAYLOAD + 1u) >> 8) & 0xffu);
    test_write_crc(body, MC_LINK_BODY_HEADER_LEN);
    encoded_len = test_cobs_wrap(body, sizeof(body), encoded, sizeof(encoded));
    ASSERT_TRUE(encoded_len > 0u);
    ASSERT_TRUE(!mc_link_decode_frame(encoded, encoded_len, &frame));
    return 0;
}

static int test_v2_decode_rejects_decoded_payload_len_mismatch(void)
{
    uint8_t body[MC_LINK_BODY_HEADER_LEN + 1u + MC_LINK_CRC_LEN];
    uint8_t encoded[32u];
    size_t encoded_len;
    mc_link_frame_t frame;

    memset(body, 0, sizeof(body));
    body[0] = MC_LINK_VERSION;
    body[1] = MC_LINK_DATA_C2M;
    body[7] = 2u;
    body[8] = 0u;
    body[MC_LINK_BODY_HEADER_LEN] = 0x99u;
    test_write_crc(body, MC_LINK_BODY_HEADER_LEN + 1u);
    encoded_len = test_cobs_wrap(body, sizeof(body), encoded, sizeof(encoded));
    ASSERT_TRUE(encoded_len > 0u);
    ASSERT_TRUE(!mc_link_decode_frame(encoded, encoded_len, &frame));
    return 0;
}

static int test_v2_parser_accepts_split_frame(void)
{
    uint8_t payload[] = {'a', 0x00u, 'c'};
    uint8_t encoded[MC_LINK_MAX_FRAME_LEN];
    size_t encoded_len = 0;
    mc_link_parser_t parser;
    mc_link_frame_t frame;
    ASSERT_TRUE(mc_link_encode(MC_LINK_DATA_C2M, 0x22u, 0x11u, payload, sizeof(payload),
                               encoded, sizeof(encoded), &encoded_len));
    mc_link_parser_init(&parser);
    ASSERT_EQ(mc_link_parser_feed(&parser, encoded, 2u, &frame), MC_LINK_PARSE_NEED_MORE);
    ASSERT_EQ(mc_link_parser_feed(&parser, encoded + 2u, encoded_len - 2u, &frame),
              MC_LINK_PARSE_FRAME);
    ASSERT_EQ(frame.type, MC_LINK_DATA_C2M);
    ASSERT_EQ(frame.seq, 0x22u);
    ASSERT_EQ(frame.ack, 0x11u);
    ASSERT_EQ(frame.len, sizeof(payload));
    ASSERT_MEMEQ(frame.payload, payload, sizeof(payload));
    return 0;
}

static int test_v2_parser_resyncs_after_corrupted_delimiter_framed_data(void)
{
    uint8_t payload[] = {0x10u, 0x20u};
    uint8_t bad[] = {0x03u, 0x01u, MC_LINK_DELIMITER};
    uint8_t good[MC_LINK_MAX_FRAME_LEN];
    uint8_t stream[sizeof(bad) + MC_LINK_MAX_FRAME_LEN];
    size_t good_len = 0;
    mc_link_parser_t parser;
    mc_link_frame_t frame;
    ASSERT_TRUE(mc_link_encode(MC_LINK_READY, 0u, 0u, payload, sizeof(payload),
                               good, sizeof(good), &good_len));
    memcpy(stream, bad, sizeof(bad));
    memcpy(stream + sizeof(bad), good, good_len);
    mc_link_parser_init(&parser);
    ASSERT_EQ(mc_link_parser_feed(&parser, stream, sizeof(bad) + good_len, &frame),
              MC_LINK_PARSE_FRAME);
    ASSERT_EQ(frame.type, MC_LINK_READY);
    ASSERT_EQ(frame.len, sizeof(payload));
    ASSERT_MEMEQ(frame.payload, payload, sizeof(payload));
    ASSERT_TRUE(parser.length_error_count + parser.crc_error_count + parser.resync_count > 0u);
    return 0;
}

static int test_v2_parser_drains_second_frame_from_buffered_input(void)
{
    uint8_t payload1[] = {0x11u};
    uint8_t payload2[] = {0x22u, 0x00u, 0x33u};
    uint8_t encoded1[MC_LINK_MAX_FRAME_LEN];
    uint8_t encoded2[MC_LINK_MAX_FRAME_LEN];
    uint8_t stream[(MC_LINK_MAX_FRAME_LEN * 2u) + 1u];
    size_t encoded1_len = 0;
    size_t encoded2_len = 0;
    mc_link_parser_t parser;
    mc_link_frame_t frame;
    ASSERT_TRUE(mc_link_encode(MC_LINK_DATA_C2M, 1u, 0u, payload1, sizeof(payload1),
                               encoded1, sizeof(encoded1), &encoded1_len));
    ASSERT_TRUE(mc_link_encode(MC_LINK_PONG, 0u, 1u, payload2, sizeof(payload2),
                               encoded2, sizeof(encoded2), &encoded2_len));
    memcpy(stream, encoded1, encoded1_len);
    memcpy(stream + encoded1_len, encoded2, encoded2_len);
    mc_link_parser_init(&parser);
    ASSERT_EQ(mc_link_parser_feed(&parser, stream, encoded1_len + encoded2_len, &frame),
              MC_LINK_PARSE_FRAME);
    ASSERT_EQ(frame.type, MC_LINK_DATA_C2M);
    ASSERT_EQ(frame.seq, 1u);
    ASSERT_EQ(frame.len, sizeof(payload1));
    ASSERT_MEMEQ(frame.payload, payload1, sizeof(payload1));
    ASSERT_EQ(mc_link_parser_feed(&parser, NULL, 0u, &frame), MC_LINK_PARSE_FRAME);
    ASSERT_EQ(frame.type, MC_LINK_PONG);
    ASSERT_EQ(frame.ack, 1u);
    ASSERT_EQ(frame.len, sizeof(payload2));
    ASSERT_MEMEQ(frame.payload, payload2, sizeof(payload2));
    return 0;
}

static int test_v2_parser_discards_after_overflow_until_delimiter(void)
{
    uint8_t junk[MC_LINK_MAX_FRAME_LEN];
    uint8_t poisoned[MC_LINK_MAX_FRAME_LEN];
    uint8_t valid[MC_LINK_MAX_FRAME_LEN];
    size_t poisoned_len = 0;
    size_t valid_len = 0;
    mc_link_parser_t parser;
    mc_link_frame_t frame;

    memset(junk, 0x55u, sizeof(junk));
    ASSERT_TRUE(mc_link_encode(MC_LINK_READY, 1u, 0u, NULL, 0u,
                               poisoned, sizeof(poisoned), &poisoned_len));
    ASSERT_TRUE(mc_link_encode(MC_LINK_PONG, 2u, 1u, NULL, 0u,
                               valid, sizeof(valid), &valid_len));

    mc_link_parser_init(&parser);
    ASSERT_EQ(mc_link_parser_feed(&parser, junk, sizeof(junk), &frame),
              MC_LINK_PARSE_NEED_MORE);
    ASSERT_EQ(mc_link_parser_feed(&parser, poisoned, poisoned_len, &frame),
              MC_LINK_PARSE_NEED_MORE);
    ASSERT_EQ(mc_link_parser_feed(&parser, valid, valid_len, &frame),
              MC_LINK_PARSE_FRAME);
    ASSERT_EQ(frame.type, MC_LINK_PONG);
    ASSERT_EQ(frame.seq, 2u);
    ASSERT_EQ(frame.ack, 1u);
    ASSERT_EQ(frame.len, 0u);
    return 0;
}

static int test_v2_parser_discards_oversized_trailing_feed_after_frame(void)
{
    uint8_t first[MC_LINK_MAX_FRAME_LEN];
    uint8_t poisoned[MC_LINK_MAX_FRAME_LEN];
    uint8_t valid[MC_LINK_MAX_FRAME_LEN];
    uint8_t stream[MC_LINK_MAX_FRAME_LEN * 2u];
    size_t first_len = 0u;
    size_t poisoned_len = 0u;
    size_t valid_len = 0u;
    mc_link_parser_t parser;
    mc_link_frame_t frame;

    ASSERT_TRUE(mc_link_encode(MC_LINK_READY, 1u, 0u, NULL, 0u,
                               first, sizeof(first), &first_len));
    ASSERT_TRUE(mc_link_encode(MC_LINK_DATA_C2M, 2u, 1u, NULL, 0u,
                               poisoned, sizeof(poisoned), &poisoned_len));
    ASSERT_TRUE(mc_link_encode(MC_LINK_PONG, 3u, 2u, NULL, 0u,
                               valid, sizeof(valid), &valid_len));
    memcpy(stream, first, first_len);
    memset(stream + first_len, 0x66u, MC_LINK_MAX_FRAME_LEN + 1u);

    mc_link_parser_init(&parser);
    ASSERT_EQ(mc_link_parser_feed(&parser, stream, first_len + MC_LINK_MAX_FRAME_LEN + 1u, &frame),
              MC_LINK_PARSE_FRAME);
    ASSERT_EQ(frame.type, MC_LINK_READY);
    ASSERT_TRUE(parser.discarding);
    ASSERT_EQ(parser.len, 0u);
    ASSERT_EQ(mc_link_parser_feed(&parser, poisoned, poisoned_len, &frame),
              MC_LINK_PARSE_NEED_MORE);
    ASSERT_EQ(mc_link_parser_feed(&parser, valid, valid_len, &frame),
              MC_LINK_PARSE_FRAME);
    ASSERT_EQ(frame.type, MC_LINK_PONG);
    ASSERT_EQ(frame.seq, 3u);
    ASSERT_EQ(frame.ack, 2u);
    return 0;
}

int test_link_codec(void)
{
    if (test_v2_golden_vector_small_frame()) return 1;
    if (test_v2_cobs_round_trip_with_zero_payload_bytes()) return 1;
    if (test_v2_decode_rejects_bad_crc()) return 1;
    if (test_v2_max_payload_fits_512_encoded_bytes()) return 1;
    if (test_v2_encode_rejects_payload_over_cap()) return 1;
    if (test_v2_empty_payload_round_trip()) return 1;
    if (test_v2_decode_rejects_missing_delimiter()) return 1;
    if (test_v2_decode_rejects_bad_version()) return 1;
    if (test_v2_cobs_round_trip_254_byte_span()) return 1;
    if (test_v2_decode_rejects_malformed_cobs()) return 1;
    if (test_v2_decode_rejects_decoded_payload_len_over_cap()) return 1;
    if (test_v2_decode_rejects_decoded_payload_len_mismatch()) return 1;
    if (test_v2_parser_accepts_split_frame()) return 1;
    if (test_v2_parser_resyncs_after_corrupted_delimiter_framed_data()) return 1;
    if (test_v2_parser_drains_second_frame_from_buffered_input()) return 1;
    if (test_v2_parser_discards_after_overflow_until_delimiter()) return 1;
    if (test_v2_parser_discards_oversized_trailing_feed_after_frame()) return 1;
    return 0;
}
