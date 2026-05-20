#include <stdint.h>
#include "mc_config.h"
#include "mc_packet.h"
#include "mc_ringbuf.h"
#include "mc_server.h"
#include "mc_varint.h"
#include "mc_world.h"
#include <string.h>
#if MC_PROTOCOL_COMPRESSION_ENABLE && MC_USE_PSRAM_COMPRESSED_MAP
#include "mc_world_compressed.h"
#endif

#define ASSERT_TRUE(expr) do { if (!(expr)) return 1; } while (0)
#define ASSERT_EQ(a, b) do { if ((a) != (b)) return 1; } while (0)

static int read_packet_id(const uint8_t *src, size_t src_len, int32_t *packet_id)
{
    mc_packet_t packet;
    size_t used = 0;
    if (!mc_packet_try_read(src, src_len, &packet)) {
        return 0;
    }
    return mc_varint_decode(packet.body, packet.body_len, packet_id, &used);
}

static size_t first_frame_len(const uint8_t *src, size_t src_len)
{
    mc_packet_t packet;
    if (!mc_packet_try_read(src, src_len, &packet)) {
        return 0u;
    }
    return packet.frame_len;
}

static int read_compressed_packet_id(const uint8_t *src, size_t src_len, int32_t *packet_id, int32_t *data_len)
{
    mc_packet_t packet;
    const uint8_t *body = 0;
    size_t body_len = 0;
    size_t used = 0;

    if (!mc_packet_try_read(src, src_len, &packet)) {
        return 0;
    }
    if (!mc_packet_get_compressed_body(packet.body, packet.body_len, &body, &body_len, data_len)) {
        return 0;
    }
    if (*data_len != 0) {
        return 0;
    }
    return mc_varint_decode(body, body_len, packet_id, &used);
}

static int read_clientbound_packet_id(const uint8_t *src, size_t src_len, int32_t *packet_id)
{
    mc_packet_t packet;
    size_t used = 0;

    if (!mc_packet_try_read(src, src_len, &packet)) {
        return 0;
    }
#if MC_PROTOCOL_COMPRESSION_ENABLE
    {
        const uint8_t *body = 0;
        size_t body_len = 0;
        int32_t data_len = 0;

        if (mc_packet_get_compressed_body(packet.body, packet.body_len, &body, &body_len, &data_len) &&
            data_len == 0) {
            return mc_varint_decode(body, body_len, packet_id, &used);
        }
    }
#endif
    return mc_varint_decode(packet.body, packet.body_len, packet_id, &used);
}

static size_t clientbound_frame_len(const uint8_t *src, size_t src_len)
{
    mc_packet_t packet;
    if (!mc_packet_try_read(src, src_len, &packet)) {
        return 0u;
    }
    return packet.frame_len;
}

static size_t make_compressed_plain_frame(const uint8_t *body, size_t body_len, uint8_t *dst, size_t dst_cap)
{
    return mc_packet_wrap_compressed_plain(body, body_len, dst, dst_cap);
}

static int count_packet_id(const uint8_t *src, size_t src_len, int32_t expected)
{
    int count = 0;
    size_t pos = 0;
    while (pos < src_len) {
        size_t frame_len = clientbound_frame_len(src + pos, src_len - pos);
        int32_t packet_id = 0;
        if (frame_len == 0u) {
            return -1;
        }
        if (!read_clientbound_packet_id(src + pos, src_len - pos, &packet_id)) {
            return -1;
        }
        if (packet_id == expected) {
            count++;
        }
        pos += frame_len;
    }
    return count;
}

static size_t drain_ring(mc_ringbuf_t *tx, uint8_t *dst, size_t dst_cap)
{
    return mc_ringbuf_read(tx, dst, dst_cap);
}

static int test_login_enables_compression_and_accepts_plain_compressed_serverbound(void)
{
    mc_server_t server;
    uint8_t tx_storage[MC_TX_RING_CAP];
    mc_ringbuf_t tx;
    uint8_t out[4096];
    uint8_t frame[64];
    size_t out_len;
    int32_t packet_id = -1;
    int32_t data_len = -1;

    const uint8_t handshake_login[] = {
        0x0f, 0x00, 0x2f, 0x09, '1','2','7','.','0','.','0','.','1', 0x63, 0xdd, 0x02
    };
    const uint8_t login_start[] = {
        0x09, 0x00, 0x07, 'p','l','a','y','e','r','1'
    };
    const uint8_t client_settings_body[] = {
        0x15, 0x02, 'e', 'n', 0x08, 0x00, 0x00, 0x01, 0x7f, 0x01
    };

    mc_server_init(&server);
    mc_ringbuf_init(&tx, tx_storage, sizeof(tx_storage));
    ASSERT_TRUE(mc_server_receive(&server, handshake_login, sizeof(handshake_login), &tx));
    ASSERT_TRUE(mc_server_receive(&server, login_start, sizeof(login_start), &tx));
    ASSERT_EQ(server.state, MC_CONN_PLAY);
    ASSERT_EQ(server.compression_enabled, MC_PROTOCOL_COMPRESSION_ENABLE ? 1 : 0);

    out_len = mc_ringbuf_read(&tx, out, sizeof(out));
#if MC_PROTOCOL_COMPRESSION_ENABLE
    {
        size_t first_len = first_frame_len(out, out_len);
        ASSERT_TRUE(first_len > 0u);
        ASSERT_TRUE(read_packet_id(out, out_len, &packet_id));
        ASSERT_EQ(packet_id, 0x03);
        ASSERT_TRUE(read_compressed_packet_id(out + first_len,
                                             out_len - first_len,
                                             &packet_id,
                                             &data_len));
    }
    ASSERT_EQ(packet_id, 0x02);
    ASSERT_EQ(data_len, 0);

    out_len = make_compressed_plain_frame(client_settings_body, sizeof(client_settings_body), frame, sizeof(frame));
    ASSERT_TRUE(out_len > 0u);
    ASSERT_TRUE(mc_server_receive(&server, frame, out_len, &tx));

    out_len = mc_packet_wrap_compressed_payload((int32_t)sizeof(client_settings_body),
                                               client_settings_body,
                                               sizeof(client_settings_body),
                                               frame,
                                               sizeof(frame));
    ASSERT_TRUE(out_len > 0u);
    ASSERT_TRUE(!mc_server_receive(&server, frame, out_len, &tx));
#else
    (void)out_len;
    (void)packet_id;
    (void)data_len;
    (void)frame;
    (void)client_settings_body;
    (void)first_frame_len;
    (void)read_compressed_packet_id;
    (void)make_compressed_plain_frame;
#endif
    return 0;
}

static int test_compressed_partial_frame_rejects_invalid_inner_packet_id(void)
{
#if MC_PROTOCOL_COMPRESSION_ENABLE
    mc_server_t server;
    uint8_t tx_storage[MC_TX_RING_CAP];
    mc_ringbuf_t tx;
    uint8_t out[4096];

    const uint8_t handshake_login[] = {
        0x0f, 0x00, 0x2f, 0x09, '1','2','7','.','0','.','0','.','1', 0x63, 0xdd, 0x02
    };
    const uint8_t login_start[] = {
        0x09, 0x00, 0x07, 'p','l','a','y','e','r','1'
    };
    const uint8_t partial_compressed_invalid_packet_id[] = {
        0x04, 0x00, 0x80, 0x01
    };
    const uint8_t partial_compressed_payload[] = {
        0x04, 0x01
    };

    mc_server_init(&server);
    mc_ringbuf_init(&tx, tx_storage, sizeof(tx_storage));
    ASSERT_TRUE(mc_server_receive(&server, handshake_login, sizeof(handshake_login), &tx));
    ASSERT_TRUE(mc_server_receive(&server, login_start, sizeof(login_start), &tx));
    ASSERT_EQ(server.state, MC_CONN_PLAY);
    (void)mc_ringbuf_read(&tx, out, sizeof(out));

    ASSERT_TRUE(!mc_server_receive(&server,
                                   partial_compressed_invalid_packet_id,
                                   sizeof(partial_compressed_invalid_packet_id),
                                   &tx));

    mc_server_init(&server);
    ASSERT_TRUE(mc_server_receive(&server, handshake_login, sizeof(handshake_login), &tx));
    ASSERT_TRUE(mc_server_receive(&server, login_start, sizeof(login_start), &tx));
    ASSERT_EQ(server.state, MC_CONN_PLAY);
    (void)mc_ringbuf_read(&tx, out, sizeof(out));

    ASSERT_TRUE(!mc_server_receive(&server,
                                   partial_compressed_payload,
                                   sizeof(partial_compressed_payload),
                                   &tx));
#endif
    return 0;
}

int test_server_login(void)
{
    mc_server_t server;
    uint8_t tx_storage[MC_TX_RING_CAP];
    uint8_t small_tx_storage[27];
    mc_ringbuf_t tx;
    mc_ringbuf_t small_tx;
    uint8_t out[4096];
    uint8_t chunk_out[MC_MAX_PACKET_BODY + 8u];
    size_t out_len;
    int32_t packet_id;

    const uint8_t queue_body[] = { 0x02, 0xaa, 0xbb };
    const uint8_t queue_large_body[] = { 0x01, 0x02, 0x03, 0x04 };
    const uint8_t handshake_login[] = {
        0x0f, 0x00, 0x2f, 0x09, '1','2','7','.','0','.','0','.','1', 0x63, 0xdd, 0x02
    };
    const uint8_t login_start[] = {
        0x09, 0x00, 0x07, 'p','l','a','y','e','r','1'
    };
    const uint8_t empty_login_start[] = {
        0x02, 0x00, 0x00
    };

    mc_ringbuf_init(&tx, tx_storage, sizeof(tx_storage));
    mc_server_init(&server);

    ASSERT_TRUE(mc_server_queue_packet(&tx, queue_body, sizeof(queue_body)));
    out_len = mc_ringbuf_read(&tx, out, sizeof(out));
    ASSERT_EQ(out_len, sizeof(queue_body) + 1u);
    ASSERT_EQ(out[0], sizeof(queue_body));
    ASSERT_TRUE(read_packet_id(out, out_len, &packet_id));
    ASSERT_EQ(packet_id, 0x02);

    mc_ringbuf_init(&small_tx, small_tx_storage, 4u);
    ASSERT_TRUE(!mc_server_queue_packet(&small_tx, queue_large_body, sizeof(queue_large_body)));
    ASSERT_EQ(mc_ringbuf_len(&small_tx), 0u);

    mc_server_init(&server);
    mc_ringbuf_init(&small_tx, small_tx_storage, sizeof(small_tx_storage));
    ASSERT_TRUE(mc_server_receive(&server, handshake_login, sizeof(handshake_login), &tx));
    ASSERT_EQ(server.state, MC_CONN_LOGIN);
    ASSERT_TRUE(!mc_server_receive(&server, login_start, sizeof(login_start), &small_tx));
    ASSERT_EQ(server.state, MC_CONN_LOGIN);
    ASSERT_TRUE(mc_server_tick(&server, &tx));
    ASSERT_EQ(server.state, MC_CONN_LOGIN);
    out_len = mc_ringbuf_read(&small_tx, out, sizeof(out));
    ASSERT_EQ(out_len, 0u);

    mc_server_init(&server);
    ASSERT_TRUE(mc_server_receive(&server, handshake_login, sizeof(handshake_login), &tx));
    ASSERT_EQ(server.state, MC_CONN_LOGIN);
    ASSERT_TRUE(!mc_server_receive(&server, empty_login_start, sizeof(empty_login_start), &tx));

    mc_server_init(&server);
    ASSERT_TRUE(mc_server_receive(&server, handshake_login, sizeof(handshake_login), &tx));
    ASSERT_EQ(server.state, MC_CONN_LOGIN);
    ASSERT_TRUE(mc_server_receive(&server, login_start, sizeof(login_start), &tx));
    ASSERT_EQ(server.state, MC_CONN_PLAY);

    out_len = mc_ringbuf_read(&tx, out, sizeof(out));
    ASSERT_TRUE(out_len > 20u);
#if MC_PROTOCOL_COMPRESSION_ENABLE
    ASSERT_TRUE(read_packet_id(out, out_len, &packet_id));
    ASSERT_EQ(packet_id, 0x03);
#else
    ASSERT_TRUE(read_packet_id(out, out_len, &packet_id));
    ASSERT_EQ(packet_id, 0x02);
#endif

    ASSERT_TRUE(mc_server_tick(&server, &tx));
    out_len = mc_ringbuf_read(&tx, out, sizeof(out));
    ASSERT_TRUE(out_len > 30u);
    ASSERT_TRUE(read_clientbound_packet_id(out, out_len, &packet_id));
    ASSERT_EQ(packet_id, 0x01);

    mc_server_init(&server);
    ASSERT_TRUE(mc_server_receive(&server, handshake_login, sizeof(handshake_login), &tx));
    ASSERT_TRUE(mc_server_receive(&server, login_start, sizeof(login_start), &tx));
    (void)mc_ringbuf_read(&tx, out, sizeof(out));
    mc_ringbuf_init(&small_tx, small_tx_storage, sizeof(small_tx_storage));
    ASSERT_TRUE(!mc_server_tick(&server, &small_tx));
    out_len = mc_ringbuf_read(&small_tx, out, sizeof(out));
    ASSERT_EQ(count_packet_id(out, out_len, 0x01), 1);
    ASSERT_TRUE(!mc_server_tick(&server, &small_tx));
    out_len = mc_ringbuf_read(&small_tx, out, sizeof(out));
    ASSERT_EQ(count_packet_id(out, out_len, 0x01), 0);

    ASSERT_TRUE(MC_TX_RING_CAP >= 49152u);
#if MC_PROTOCOL_COMPRESSION_ENABLE && MC_USE_PSRAM_COMPRESSED_MAP
    {
        static uint8_t compressed_arena[4096];
        ASSERT_TRUE(mc_world_compressed_init(compressed_arena, sizeof(compressed_arena)));
    }
#endif
    mc_server_init(&server);
    mc_ringbuf_init(&tx, tx_storage, sizeof(tx_storage));
    ASSERT_TRUE(mc_server_receive(&server, handshake_login, sizeof(handshake_login), &tx));
    ASSERT_TRUE(mc_server_receive(&server, login_start, sizeof(login_start), &tx));
    (void)drain_ring(&tx, out, sizeof(out));
    ASSERT_TRUE(mc_server_tick(&server, &tx));
    out_len = drain_ring(&tx, chunk_out, sizeof(chunk_out));
    ASSERT_TRUE(out_len > 30u);
    ASSERT_EQ(count_packet_id(chunk_out, out_len, 0x21), 0);
#if MC_PROTOCOL_COMPRESSION_ENABLE && MC_USE_PSRAM_COMPRESSED_MAP
    ASSERT_TRUE(mc_world_compressed_ready());
    for (size_t i = 0; i < 9u; i++) {
        mc_packet_t compressed_packet;
        const mc_world_compressed_chunk_t *chunk = mc_world_compressed_chunk(i);
        const uint8_t *compressed = 0;
        size_t compressed_len = 0;
        int32_t data_len = 0;

        ASSERT_TRUE(chunk != 0);
        ASSERT_TRUE(mc_server_tick(&server, &tx));
        out_len = drain_ring(&tx, chunk_out, sizeof(chunk_out));
        ASSERT_TRUE(out_len > 0u);
        ASSERT_TRUE(mc_packet_try_read(chunk_out, out_len, &compressed_packet));
        ASSERT_EQ(compressed_packet.frame_len, out_len);
        ASSERT_TRUE(mc_packet_get_compressed_body(compressed_packet.body,
                                                 compressed_packet.body_len,
                                                 &compressed,
                                                 &compressed_len,
                                                 &data_len));
        ASSERT_EQ(data_len, (int32_t)chunk->raw_body_len);
        ASSERT_TRUE(data_len > (int32_t)MC_CHUNK_SECTION_BYTES);
        ASSERT_EQ(compressed_len, chunk->compressed_len);
        ASSERT_TRUE(compressed_len < (size_t)data_len);
        ASSERT_TRUE(memcmp(compressed, chunk->compressed, compressed_len) == 0);
    }
#elif MC_PROTOCOL_COMPRESSION_ENABLE
    for (size_t i = 0; i < 9u; i++) {
        mc_packet_t compressed_packet;
        const uint8_t *body = 0;
        size_t body_len = 0;
        int32_t data_len = -1;

        ASSERT_TRUE(mc_server_tick(&server, &tx));
        out_len = drain_ring(&tx, chunk_out, sizeof(chunk_out));
        ASSERT_TRUE(out_len > MC_CHUNK_SECTION_BYTES);
        ASSERT_TRUE(mc_packet_try_read(chunk_out, out_len, &compressed_packet));
        ASSERT_EQ(compressed_packet.frame_len, out_len);
        ASSERT_TRUE(mc_packet_get_compressed_body(compressed_packet.body,
                                                 compressed_packet.body_len,
                                                 &body,
                                                 &body_len,
                                                 &data_len));
        ASSERT_EQ(data_len, 0);
        ASSERT_TRUE(body_len > MC_CHUNK_SECTION_BYTES);
        ASSERT_EQ(count_packet_id(chunk_out, out_len, 0x21), 1);
    }
#else
    for (size_t i = 0; i < 9u; i++) {
        ASSERT_TRUE(mc_server_tick(&server, &tx));
        out_len = drain_ring(&tx, chunk_out, sizeof(chunk_out));
        ASSERT_TRUE(out_len > MC_CHUNK_SECTION_BYTES);
        ASSERT_EQ(count_packet_id(chunk_out, out_len, 0x21), 1);
    }
#endif
    ASSERT_TRUE(mc_server_tick(&server, &tx));
    out_len = drain_ring(&tx, chunk_out, sizeof(chunk_out));
    ASSERT_EQ(out_len, 0u);

    ASSERT_TRUE(!test_login_enables_compression_and_accepts_plain_compressed_serverbound());
    ASSERT_TRUE(!test_compressed_partial_frame_rejects_invalid_inner_packet_id());

    return 0;
}
