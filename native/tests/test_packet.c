#include <stdint.h>
#include <string.h>
#include "mc_packet.h"

#define ASSERT_TRUE(expr) do { if (!(expr)) return 1; } while (0)
#define ASSERT_EQ(a, b) do { if ((a) != (b)) return 1; } while (0)

static int test_compressed_plain_packet_frame(void)
{
    uint8_t out[16];
    const uint8_t body[] = { 0x03, 0x40, 0x00 };
    size_t n = mc_packet_wrap_compressed_plain(body, sizeof(body), out, sizeof(out));

    ASSERT_EQ(n, 5u);
    ASSERT_EQ(out[0], 0x04u);
    ASSERT_EQ(out[1], 0x00u);
    ASSERT_EQ(out[2], 0x03u);
    ASSERT_EQ(out[3], 0x40u);
    ASSERT_EQ(out[4], 0x00u);
    return 0;
}

static int test_compressed_payload_packet_frame(void)
{
    uint8_t out[32];
    const uint8_t payload[] = { 0x78, 0x9c, 0x63, 0x60 };
    size_t n = mc_packet_wrap_compressed_payload(12558u, payload, sizeof(payload), out, sizeof(out));

    ASSERT_EQ(n, 7u);
    ASSERT_EQ(out[0], 0x06u);
    ASSERT_EQ(out[1], 0x8e);
    ASSERT_EQ(out[2], 0x62);
    ASSERT_EQ(out[3], 0x78);
    ASSERT_EQ(out[4], 0x9c);
    ASSERT_EQ(out[5], 0x63);
    ASSERT_EQ(out[6], 0x60);
    return 0;
}

static int test_packet_frame_len_helpers(void)
{
    ASSERT_EQ(mc_packet_frame_len(3u), 4u);
    ASSERT_EQ(mc_packet_compressed_plain_frame_len(3u), 5u);
    ASSERT_EQ(mc_packet_compressed_payload_frame_len(12558, 4u), 7u);
    return 0;
}

static int test_parse_compressed_plain_body(void)
{
    const uint8_t frame_body[] = { 0x00, 0x01, 0x02, 0x03 };
    const uint8_t *body = 0;
    size_t body_len = 0;
    int32_t data_len = -1;

    ASSERT_TRUE(mc_packet_get_compressed_body(frame_body, sizeof(frame_body), &body, &body_len, &data_len));
    ASSERT_EQ(data_len, 0);
    ASSERT_EQ(body_len, 3u);
    ASSERT_EQ(body[0], 0x01u);
    ASSERT_EQ(body[2], 0x03u);
    return 0;
}

static int test_parse_compressed_payload_body_rejected_by_caller(void)
{
    const uint8_t frame_body[] = { 0x8e, 0x62, 0x78, 0x9c };
    const uint8_t *body = 0;
    size_t body_len = 0;
    int32_t data_len = -1;

    ASSERT_TRUE(mc_packet_get_compressed_body(frame_body, sizeof(frame_body), &body, &body_len, &data_len));
    ASSERT_EQ(data_len, 12558);
    ASSERT_EQ(body_len, 2u);
    ASSERT_EQ(body[0], 0x78u);
    return 0;
}

int test_packet(void)
{
    uint8_t out[64];
    uint8_t body[8];
    uint8_t src[8];
    mc_packet_t pkt;
    mc_writer_t w;
    size_t len = 0;

    body[0] = 0x00;
    body[1] = 0x01;
    body[2] = 0x02;
    len = mc_packet_wrap(body, 3, out, sizeof(out));
    ASSERT_EQ(len, 4u);
    ASSERT_EQ(out[0], 0x03u);
    ASSERT_EQ(out[1], 0x00u);
    ASSERT_EQ(out[2], 0x01u);
    ASSERT_EQ(out[3], 0x02u);

    ASSERT_TRUE(mc_packet_try_read(out, len, &pkt));
    ASSERT_EQ(pkt.frame_len, 4u);
    ASSERT_EQ(pkt.body_len, 3u);
    ASSERT_EQ(pkt.body[0], 0x00u);
    ASSERT_EQ(pkt.body[2], 0x02u);

    ASSERT_TRUE(!mc_packet_try_read(out, 2, &pkt));

    out[0] = 0x80;
    out[1] = 0x80;
    out[2] = 0x80;
    out[3] = 0x80;
    out[4] = 0x10;
    ASSERT_TRUE(!mc_packet_try_read(out, 5, &pkt));

    ASSERT_EQ(mc_packet_wrap(body, MC_MAX_PACKET_BODY + 1u, out, sizeof(out)), 0u);
    ASSERT_EQ(mc_packet_wrap(body, 3, out, 3), 0u);

    mc_writer_init(&w, out, sizeof(out));
    ASSERT_TRUE(mc_write_u16(&w, 0x1234u));
    ASSERT_EQ(w.len, 2u);
    ASSERT_EQ(out[0], 0x12u);
    ASSERT_EQ(out[1], 0x34u);

    mc_writer_init(&w, out, sizeof(out));
    ASSERT_TRUE(mc_write_i32(&w, 0x12345678));
    ASSERT_EQ(w.len, 4u);
    ASSERT_EQ(out[0], 0x12u);
    ASSERT_EQ(out[1], 0x34u);
    ASSERT_EQ(out[2], 0x56u);
    ASSERT_EQ(out[3], 0x78u);

    mc_writer_init(&w, out, sizeof(out));
    ASSERT_TRUE(mc_write_i64(&w, 0x0102030405060708LL));
    ASSERT_EQ(w.len, 8u);
    ASSERT_EQ(out[0], 0x01u);
    ASSERT_EQ(out[1], 0x02u);
    ASSERT_EQ(out[2], 0x03u);
    ASSERT_EQ(out[3], 0x04u);
    ASSERT_EQ(out[4], 0x05u);
    ASSERT_EQ(out[5], 0x06u);
    ASSERT_EQ(out[6], 0x07u);
    ASSERT_EQ(out[7], 0x08u);

    mc_writer_init(&w, out, sizeof(out));
    ASSERT_TRUE(mc_write_bool(&w, 1));
    ASSERT_TRUE(mc_write_bool(&w, 0));
    ASSERT_EQ(w.len, 2u);
    ASSERT_EQ(out[0], 0x01u);
    ASSERT_EQ(out[1], 0x00u);

    mc_writer_init(&w, out, sizeof(out));
    ASSERT_TRUE(mc_write_varint(&w, 300));
    ASSERT_EQ(w.len, 2u);
    ASSERT_EQ(out[0], 0xACu);
    ASSERT_EQ(out[1], 0x02u);

    src[0] = 0xAA;
    src[1] = 0xBB;
    src[2] = 0xCC;
    mc_writer_init(&w, out, sizeof(out));
    ASSERT_TRUE(mc_write_bytes(&w, src, 3));
    ASSERT_EQ(w.len, 3u);
    ASSERT_EQ(out[0], 0xAAu);
    ASSERT_EQ(out[1], 0xBBu);
    ASSERT_EQ(out[2], 0xCCu);

    mc_writer_init(&w, out, sizeof(out));
    ASSERT_TRUE(mc_write_string(&w, "abc"));
    ASSERT_EQ(w.len, 4u);
    ASSERT_EQ(out[0], 0x03u);
    ASSERT_EQ(out[1], 'a');
    ASSERT_EQ(out[2], 'b');
    ASSERT_EQ(out[3], 'c');

    mc_writer_init(&w, out, 1);
    ASSERT_TRUE(!mc_write_u16(&w, 0x1234u));
    ASSERT_EQ(w.len, 0u);

    mc_writer_init(&w, out, 3);
    ASSERT_TRUE(!mc_write_i32(&w, 0x12345678));
    ASSERT_EQ(w.len, 0u);

    mc_writer_init(&w, out, 4);
    ASSERT_TRUE(!mc_write_varint(&w, -1));
    ASSERT_EQ(w.len, 0u);

    mc_writer_init(&w, out, 1);
    ASSERT_TRUE(!mc_write_bytes(&w, src, 2));
    ASSERT_EQ(w.len, 0u);

    mc_writer_init(&w, out, 1);
    w.len = 2;
    ASSERT_TRUE(!mc_write_bytes(&w, src, 0));
    ASSERT_EQ(w.len, 2u);

    mc_writer_init(&w, out, 3);
    ASSERT_TRUE(!mc_write_string(&w, "abc"));
    ASSERT_EQ(w.len, 0u);

    if (test_compressed_plain_packet_frame()) return 1;
    if (test_compressed_payload_packet_frame()) return 1;
    if (test_packet_frame_len_helpers()) return 1;
    if (test_parse_compressed_plain_body()) return 1;
    if (test_parse_compressed_payload_body_rejected_by_caller()) return 1;

    return 0;
}
