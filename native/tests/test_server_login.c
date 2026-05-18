#include <stdint.h>
#include "mc_config.h"
#include "mc_packet.h"
#include "mc_ringbuf.h"
#include "mc_server.h"
#include "mc_varint.h"
#include "mc_world.h"

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

static int count_packet_id(const uint8_t *src, size_t src_len, int32_t expected)
{
    int count = 0;
    size_t pos = 0;
    while (pos < src_len) {
        mc_packet_t packet;
        int32_t packet_id = 0;
        size_t used = 0;
        if (!mc_packet_try_read(src + pos, src_len - pos, &packet)) {
            return -1;
        }
        if (!mc_varint_decode(packet.body, packet.body_len, &packet_id, &used)) {
            return -1;
        }
        if (packet_id == expected) {
            count++;
        }
        pos += packet.frame_len;
    }
    return count;
}

static size_t drain_ring(mc_ringbuf_t *tx, uint8_t *dst, size_t dst_cap)
{
    return mc_ringbuf_read(tx, dst, dst_cap);
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
    out_len = mc_ringbuf_read(&tx, out, sizeof(out));
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
    ASSERT_TRUE(read_packet_id(out, out_len, &packet_id));
    ASSERT_EQ(packet_id, 0x02);

    ASSERT_TRUE(mc_server_tick(&server, &tx));
    out_len = mc_ringbuf_read(&tx, out, sizeof(out));
    ASSERT_TRUE(out_len > 30u);
    ASSERT_TRUE(read_packet_id(out, out_len, &packet_id));
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
    mc_server_init(&server);
    mc_ringbuf_init(&tx, tx_storage, sizeof(tx_storage));
    ASSERT_TRUE(mc_server_receive(&server, handshake_login, sizeof(handshake_login), &tx));
    ASSERT_TRUE(mc_server_receive(&server, login_start, sizeof(login_start), &tx));
    (void)drain_ring(&tx, out, sizeof(out));
    ASSERT_TRUE(mc_server_tick(&server, &tx));
    out_len = drain_ring(&tx, chunk_out, sizeof(chunk_out));
    ASSERT_TRUE(out_len > 30u);
    ASSERT_EQ(count_packet_id(chunk_out, out_len, 0x21), 0);
    for (size_t i = 0; i < 9u; i++) {
        ASSERT_TRUE(mc_server_tick(&server, &tx));
        out_len = drain_ring(&tx, chunk_out, sizeof(chunk_out));
        ASSERT_TRUE(out_len > MC_CHUNK_SECTION_BYTES);
        ASSERT_EQ(count_packet_id(chunk_out, out_len, 0x21), 1);
    }
    ASSERT_TRUE(mc_server_tick(&server, &tx));
    out_len = drain_ring(&tx, chunk_out, sizeof(chunk_out));
    ASSERT_EQ(out_len, 0u);

    return 0;
}
