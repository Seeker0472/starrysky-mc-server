#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include "mc_link.h"

#define ASSERT_TRUE(expr) do { if (!(expr)) return 1; } while (0)
#define ASSERT_EQ(a, b) do { if ((a) != (b)) return 1; } while (0)
#define ASSERT_MEMEQ(a, b, n) do { if (memcmp((a), (b), (n)) != 0) return 1; } while (0)

static int test_crc_vector(void)
{
    static const uint8_t bytes[] = {
        MC_LINK_HELLO, 0x00, 0x05, 0x00, 0x01, 0x00, 0x02, 0x00, 0x02
    };
    ASSERT_EQ(mc_link_crc16(bytes, sizeof(bytes)), 0x6e85u);
    return 0;
}

static int test_encode_hello_exact_bytes(void)
{
    uint8_t payload[] = {1u, 0x00u, 0x02u, 0x00u, 0x02u};
    uint8_t out[32];
    size_t out_len = 0;
    static const uint8_t expected[] = {
        0x4du, 0x55u, 0x01u, 0x00u, 0x05u, 0x00u,
        0x01u, 0x00u, 0x02u, 0x00u, 0x02u,
        0x85u, 0x6eu
    };
    ASSERT_TRUE(mc_link_encode(MC_LINK_HELLO, 0, payload, sizeof(payload), out, sizeof(out), &out_len));
    ASSERT_EQ(out_len, sizeof(expected));
    ASSERT_MEMEQ(out, expected, sizeof(expected));
    return 0;
}

static int test_parser_accepts_split_frame(void)
{
    uint8_t payload[] = {'a', 'b', 'c'};
    uint8_t encoded[32];
    size_t encoded_len = 0;
    mc_link_parser_t parser;
    mc_link_frame_t frame;
    ASSERT_TRUE(mc_link_encode(MC_LINK_DATA_C2M, 7, payload, sizeof(payload), encoded, sizeof(encoded), &encoded_len));
    mc_link_parser_init(&parser);
    ASSERT_EQ(mc_link_parser_feed(&parser, encoded, 4, &frame), MC_LINK_PARSE_NEED_MORE);
    ASSERT_EQ(mc_link_parser_feed(&parser, encoded + 4, encoded_len - 4, &frame), MC_LINK_PARSE_FRAME);
    ASSERT_EQ(frame.type, MC_LINK_DATA_C2M);
    ASSERT_EQ(frame.seq, 7u);
    ASSERT_EQ(frame.len, 3u);
    ASSERT_MEMEQ(frame.payload, payload, sizeof(payload));
    return 0;
}

static int test_parser_resyncs_after_junk(void)
{
    uint8_t payload[] = {0x10u, 0x20u};
    uint8_t encoded[32];
    uint8_t stream[40];
    size_t encoded_len = 0;
    mc_link_parser_t parser;
    mc_link_frame_t frame;
    ASSERT_TRUE(mc_link_encode(MC_LINK_CREDIT, 0, payload, sizeof(payload), encoded, sizeof(encoded), &encoded_len));
    stream[0] = 0x00u;
    stream[1] = 0x4du;
    stream[2] = 0x00u;
    memcpy(stream + 3, encoded, encoded_len);
    mc_link_parser_init(&parser);
    ASSERT_EQ(mc_link_parser_feed(&parser, stream, encoded_len + 3u, &frame), MC_LINK_PARSE_FRAME);
    ASSERT_EQ(frame.type, MC_LINK_CREDIT);
    ASSERT_EQ(frame.len, 2u);
    return 0;
}

static int test_parser_recovers_after_crc_mismatch(void)
{
    uint8_t payload[] = {0x55u};
    uint8_t bad[32];
    uint8_t good[32];
    uint8_t stream[64];
    size_t bad_len = 0;
    size_t good_len = 0;
    mc_link_parser_t parser;
    mc_link_frame_t frame;
    ASSERT_TRUE(mc_link_encode(MC_LINK_DATA_C2M, 1, payload, sizeof(payload), bad, sizeof(bad), &bad_len));
    ASSERT_TRUE(mc_link_encode(MC_LINK_READY, 0, payload, sizeof(payload), good, sizeof(good), &good_len));
    bad[bad_len - 1u] ^= 0xffu;
    memcpy(stream, bad, bad_len);
    memcpy(stream + bad_len, good, good_len);
    mc_link_parser_init(&parser);
    ASSERT_EQ(mc_link_parser_feed(&parser, stream, bad_len + good_len, &frame), MC_LINK_PARSE_FRAME);
    ASSERT_EQ(frame.type, MC_LINK_READY);
    ASSERT_EQ(parser.crc_error_count, 1u);
    return 0;
}

static int test_parser_drains_second_frame_from_buffered_input(void)
{
    uint8_t payload1[] = {0x11u};
    uint8_t payload2[] = {0x22u, 0x33u};
    uint8_t encoded1[32];
    uint8_t encoded2[32];
    uint8_t stream[64];
    size_t encoded1_len = 0;
    size_t encoded2_len = 0;
    mc_link_parser_t parser;
    mc_link_frame_t frame;
    ASSERT_TRUE(mc_link_encode(MC_LINK_DATA_C2M, 1, payload1, sizeof(payload1), encoded1, sizeof(encoded1), &encoded1_len));
    ASSERT_TRUE(mc_link_encode(MC_LINK_READY, 0, payload2, sizeof(payload2), encoded2, sizeof(encoded2), &encoded2_len));
    memcpy(stream, encoded1, encoded1_len);
    memcpy(stream + encoded1_len, encoded2, encoded2_len);
    mc_link_parser_init(&parser);
    ASSERT_EQ(mc_link_parser_feed(&parser, stream, encoded1_len + encoded2_len, &frame), MC_LINK_PARSE_FRAME);
    ASSERT_EQ(frame.type, MC_LINK_DATA_C2M);
    ASSERT_EQ(frame.seq, 1u);
    ASSERT_EQ(frame.len, sizeof(payload1));
    ASSERT_MEMEQ(frame.payload, payload1, sizeof(payload1));
    ASSERT_EQ(mc_link_parser_feed(&parser, NULL, 0, &frame), MC_LINK_PARSE_FRAME);
    ASSERT_EQ(frame.type, MC_LINK_READY);
    ASSERT_EQ(frame.seq, 0u);
    ASSERT_EQ(frame.len, sizeof(payload2));
    ASSERT_MEMEQ(frame.payload, payload2, sizeof(payload2));
    return 0;
}

static int test_parser_drains_complete_frame_before_large_followup_errors(void)
{
    uint8_t payload1[MC_LINK_FIRMWARE_PAYLOAD_CAP];
    uint8_t encoded1[MC_LINK_MAX_FRAME_LEN];
    uint8_t stream[MC_LINK_MAX_FRAME_LEN + 1u];
    size_t encoded1_len = 0;
    mc_link_parser_t parser;
    mc_link_frame_t frame;
    memset(payload1, 0x5a, sizeof(payload1));
    ASSERT_TRUE(mc_link_encode(MC_LINK_DATA_C2M, 9, payload1, sizeof(payload1), encoded1, sizeof(encoded1), &encoded1_len));
    ASSERT_EQ(encoded1_len, MC_LINK_MAX_FRAME_LEN);
    memcpy(stream, encoded1, encoded1_len);
    stream[encoded1_len] = 0x00u;
    mc_link_parser_init(&parser);
    ASSERT_EQ(mc_link_parser_feed(&parser, stream, encoded1_len + 1u, &frame), MC_LINK_PARSE_FRAME);
    ASSERT_EQ(frame.type, MC_LINK_DATA_C2M);
    ASSERT_EQ(frame.seq, 9u);
    ASSERT_EQ(frame.len, MC_LINK_FIRMWARE_PAYLOAD_CAP);
    ASSERT_MEMEQ(frame.payload, payload1, sizeof(payload1));
    return 0;
}

static int test_parser_drains_partial_max_frame_before_large_next_read(void)
{
    uint8_t payload1[MC_LINK_FIRMWARE_PAYLOAD_CAP];
    uint8_t payload2[] = {'o', 'k'};
    uint8_t encoded1[MC_LINK_MAX_FRAME_LEN];
    uint8_t encoded2[32];
    uint8_t next_read[MC_LINK_MAX_FRAME_LEN + 32u];
    size_t encoded1_len = 0;
    size_t encoded2_len = 0;
    size_t split = 4u;
    mc_link_parser_t parser;
    mc_link_frame_t frame;

    for (size_t i = 0u; i < sizeof(payload1); ++i) {
        payload1[i] = (uint8_t)(i ^ 0x5au);
    }

    ASSERT_TRUE(mc_link_encode(MC_LINK_DATA_C2M, 42, payload1, sizeof(payload1), encoded1, sizeof(encoded1), &encoded1_len));
    ASSERT_TRUE(mc_link_encode(MC_LINK_READY, 0, payload2, sizeof(payload2), encoded2, sizeof(encoded2), &encoded2_len));
    ASSERT_EQ(encoded1_len, MC_LINK_MAX_FRAME_LEN);
    memcpy(next_read, encoded1 + split, encoded1_len - split);
    memcpy(next_read + encoded1_len - split, encoded2, encoded2_len);

    mc_link_parser_init(&parser);
    ASSERT_EQ(mc_link_parser_feed(&parser, encoded1, split, &frame), MC_LINK_PARSE_NEED_MORE);
    ASSERT_EQ(mc_link_parser_feed(&parser, next_read, encoded1_len - split + encoded2_len, &frame), MC_LINK_PARSE_FRAME);
    ASSERT_EQ(frame.type, MC_LINK_DATA_C2M);
    ASSERT_EQ(frame.seq, 42u);
    ASSERT_EQ(frame.len, MC_LINK_FIRMWARE_PAYLOAD_CAP);
    ASSERT_MEMEQ(frame.payload, payload1, sizeof(payload1));
    ASSERT_EQ(mc_link_parser_feed(&parser, NULL, 0, &frame), MC_LINK_PARSE_FRAME);
    ASSERT_EQ(frame.type, MC_LINK_READY);
    ASSERT_EQ(frame.seq, 0u);
    ASSERT_EQ(frame.len, sizeof(payload2));
    ASSERT_MEMEQ(frame.payload, payload2, sizeof(payload2));
    return 0;
}

static int test_parser_keeps_first_ready_frame_before_buffer_fills_again(void)
{
    uint8_t payload1[] = {'f', 'i', 'r', 's', 't'};
    uint8_t payload2[MC_LINK_FIRMWARE_PAYLOAD_CAP];
    uint8_t encoded1[32];
    uint8_t encoded2[MC_LINK_MAX_FRAME_LEN];
    uint8_t stream[sizeof(encoded1) + sizeof(encoded2) + 1u];
    size_t encoded1_len = 0;
    size_t encoded2_len = 0;
    size_t stream_len = 0;
    mc_link_parser_t parser;
    mc_link_frame_t frame;

    for (size_t i = 0u; i < sizeof(payload2); ++i) {
        payload2[i] = (uint8_t)(0xa0u ^ i);
    }

    ASSERT_TRUE(mc_link_encode(MC_LINK_READY, 0, payload1, sizeof(payload1), encoded1, sizeof(encoded1), &encoded1_len));
    ASSERT_TRUE(mc_link_encode(MC_LINK_DATA_C2M, 77, payload2, sizeof(payload2), encoded2, sizeof(encoded2), &encoded2_len));
    memcpy(stream, encoded1, encoded1_len);
    stream_len += encoded1_len;
    memcpy(stream + stream_len, encoded2, encoded2_len);
    stream_len += encoded2_len;
    stream[stream_len++] = 0x00u;

    mc_link_parser_init(&parser);
    ASSERT_EQ(mc_link_parser_feed(&parser, stream, stream_len, &frame), MC_LINK_PARSE_FRAME);
    ASSERT_EQ(frame.type, MC_LINK_READY);
    ASSERT_EQ(frame.seq, 0u);
    ASSERT_EQ(frame.len, sizeof(payload1));
    ASSERT_MEMEQ(frame.payload, payload1, sizeof(payload1));
    ASSERT_EQ(mc_link_parser_feed(&parser, NULL, 0, &frame), MC_LINK_PARSE_FRAME);
    ASSERT_EQ(frame.type, MC_LINK_DATA_C2M);
    ASSERT_EQ(frame.seq, 77u);
    ASSERT_EQ(frame.len, MC_LINK_FIRMWARE_PAYLOAD_CAP);
    ASSERT_MEMEQ(frame.payload, payload2, sizeof(payload2));
    return 0;
}

static int test_encode_rejects_payload_over_cap(void)
{
    uint8_t payload[MC_LINK_FIRMWARE_PAYLOAD_CAP + 1u];
    uint8_t out[MC_LINK_MAX_FRAME_LEN + 8u];
    size_t out_len = 0;
    memset(payload, 0xa5, sizeof(payload));
    ASSERT_TRUE(!mc_link_encode(MC_LINK_DATA_C2M, 0, payload, sizeof(payload), out, sizeof(out), &out_len));
    ASSERT_EQ(out_len, 0u);
    return 0;
}

int test_link_codec(void)
{
    if (test_crc_vector()) return 1;
    if (test_encode_hello_exact_bytes()) return 1;
    if (test_parser_accepts_split_frame()) return 1;
    if (test_parser_resyncs_after_junk()) return 1;
    if (test_parser_recovers_after_crc_mismatch()) return 1;
    if (test_parser_drains_second_frame_from_buffered_input()) return 1;
    if (test_parser_drains_complete_frame_before_large_followup_errors()) return 1;
    if (test_parser_drains_partial_max_frame_before_large_next_read()) return 1;
    if (test_parser_keeps_first_ready_frame_before_buffer_fills_again()) return 1;
    if (test_encode_rejects_payload_over_cap()) return 1;
    return 0;
}
