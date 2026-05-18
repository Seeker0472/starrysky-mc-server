#define _GNU_SOURCE
#include <stdint.h>
#include <string.h>
#include "mc_config.h"
#include "mc_packet.h"
#include "mc_ringbuf.h"
#include "mc_server.h"
#include "mc_varint.h"

#define ASSERT_TRUE(expr) do { if (!(expr)) return 1; } while (0)
#define ASSERT_EQ(a, b) do { if ((a) != (b)) return 1; } while (0)

static size_t drain(mc_ringbuf_t *rb, uint8_t *dst, size_t cap)
{
    return mc_ringbuf_read(rb, dst, cap);
}

static int packet_id_at(const uint8_t *src, size_t src_len, int32_t *packet_id)
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

int test_server_status(void)
{
    mc_server_t server;
    mc_server_t other;
    uint8_t tx_storage[MC_TX_RING_CAP];
    mc_ringbuf_t tx;
    uint8_t out[MC_TX_RING_CAP];
    size_t out_len;
    int32_t packet_id;

    const uint8_t handshake_status[] = {
        0x0f, 0x00, 0x2f, 0x09, '1','2','7','.','0','.','0','.','1', 0x63, 0xdd, 0x01
    };
    const uint8_t handshake_status_part1[] = {
        0x0f, 0x00, 0x2f, 0x09, '1','2','7'
    };
    const uint8_t handshake_status_part2[] = {
        '.','0','.','0','.','1', 0x63, 0xdd, 0x01
    };
    const uint8_t handshake_invalid_state[] = {
        0x0f, 0x00, 0x2f, 0x09, '1','2','7','.','0','.','0','.','1', 0x63, 0xdd, 0x03
    };
    const uint8_t handshake_login[] = {
        0x0f, 0x00, 0x2f, 0x09, '1','2','7','.','0','.','0','.','1', 0x63, 0xdd, 0x02
    };
    const uint8_t login_start[] = {
        0x09, 0x00, 0x07, 'p','l','a','y','e','r','1'
    };
    const uint8_t legacy_reset_magic[] = {
        0xff, 0x00, 0xff, 'M', 'C', 'U', 'R', 'S', 'T', 0x7e
    };
    const uint8_t framed_payload_with_legacy_reset_magic[] = {
        0x0b, 0x02, 0xff, 0x00, 0xff, 'M', 'C', 'U', 'R', 'S', 'T', 0x7e
    };
    const uint8_t status_request[] = { 0x01, 0x00 };
    const uint8_t status_ping[] = {
        0x09, 0x01, 0,0,0,0,0,0,0,7
    };
    const uint8_t invalid_varint_prefix[] = { 0x80, 0x80, 0x80, 0x80, 0x80, 0x00 };
    const uint8_t oversized_prefix[] = { 0xff, 0xff, 0x07 };

    mc_ringbuf_init(&tx, tx_storage, sizeof(tx_storage));
    mc_server_init(&server);

    ASSERT_TRUE(mc_server_receive(&server, handshake_status_part1, sizeof(handshake_status_part1), &tx));
    mc_server_init(&other);
    ASSERT_TRUE(mc_server_receive(&server, handshake_status_part2, sizeof(handshake_status_part2), &tx));
    ASSERT_EQ(server.state, MC_CONN_STATUS);
    ASSERT_EQ(other.state, MC_CONN_HANDSHAKE);

    mc_server_init(&server);
    ASSERT_TRUE(!mc_server_receive(&server, handshake_invalid_state, sizeof(handshake_invalid_state), &tx));
    ASSERT_EQ(server.state, MC_CONN_HANDSHAKE);

    mc_server_init(&server);
    ASSERT_TRUE(!mc_server_receive(&server, invalid_varint_prefix, sizeof(invalid_varint_prefix), &tx));
    mc_server_init(&server);
    ASSERT_TRUE(!mc_server_receive(&server, oversized_prefix, sizeof(oversized_prefix), &tx));
    mc_server_init(&server);
    ASSERT_TRUE(mc_server_receive(&server, handshake_status, sizeof(handshake_status), &tx));
    ASSERT_EQ(server.state, MC_CONN_STATUS);
    ASSERT_TRUE(mc_server_receive(&server, status_request, sizeof(status_request), &tx));
    out_len = drain(&tx, out, sizeof(out));
    ASSERT_TRUE(out_len > 20u);
    ASSERT_TRUE(packet_id_at(out, out_len, &packet_id));
    ASSERT_EQ(packet_id, 0x00);
    ASSERT_TRUE(memmem(out, out_len, "MC UART", 7) != 0);

    ASSERT_TRUE(mc_server_receive(&server, status_ping, sizeof(status_ping), &tx));
    out_len = drain(&tx, out, sizeof(out));
    ASSERT_EQ(out_len, 10u);
    ASSERT_TRUE(packet_id_at(out, out_len, &packet_id));
    ASSERT_EQ(packet_id, 0x01);
    ASSERT_EQ(out[9], 0x07u);

    mc_server_init(&server);
    mc_ringbuf_init(&tx, tx_storage, sizeof(tx_storage));
    ASSERT_TRUE(mc_server_receive(&server, handshake_status, sizeof(handshake_status), &tx));
    ASSERT_EQ(server.state, MC_CONN_STATUS);
    ASSERT_TRUE(mc_server_receive(&server, status_request, sizeof(status_request), &tx));
    out_len = drain(&tx, out, sizeof(out));
    ASSERT_TRUE(out_len > 20u);

    ASSERT_TRUE(mc_server_receive(&server, handshake_login, sizeof(handshake_login), &tx));
    ASSERT_EQ(server.state, MC_CONN_LOGIN);
    ASSERT_TRUE(mc_server_receive(&server, login_start, sizeof(login_start), &tx));
    ASSERT_EQ(server.state, MC_CONN_PLAY);

    ASSERT_TRUE(mc_server_tick(&server, &tx));
    ASSERT_TRUE(mc_server_tick(&server, &tx));

    out_len = drain(&tx, out, sizeof(out));
    ASSERT_EQ(count_packet_id(out, out_len, 0x02), 1);
    ASSERT_EQ(count_packet_id(out, out_len, 0x01), 1);
    ASSERT_EQ(count_packet_id(out, out_len, 0x21), 1);

    mc_server_init(&server);
    mc_ringbuf_init(&tx, tx_storage, sizeof(tx_storage));
    ASSERT_TRUE(mc_server_receive(&server, handshake_status, sizeof(handshake_status), &tx));
    ASSERT_EQ(server.state, MC_CONN_STATUS);
    ASSERT_TRUE(mc_server_receive(&server, status_request, sizeof(status_request), &tx));
    ASSERT_TRUE(mc_ringbuf_len(&tx) > 20u);

    ASSERT_TRUE(mc_server_receive(&server, handshake_login, sizeof(handshake_login), &tx));
    ASSERT_EQ(server.state, MC_CONN_LOGIN);
    ASSERT_TRUE(mc_server_receive(&server, login_start, sizeof(login_start), &tx));
    ASSERT_EQ(server.state, MC_CONN_PLAY);

    ASSERT_TRUE(mc_server_tick(&server, &tx));
    ASSERT_TRUE(mc_server_tick(&server, &tx));

    out_len = drain(&tx, out, sizeof(out));
    ASSERT_TRUE(packet_id_at(out, out_len, &packet_id));
    ASSERT_EQ(packet_id, 0x02);
    ASSERT_EQ(count_packet_id(out, out_len, 0x00), 0);
    ASSERT_EQ(count_packet_id(out, out_len, 0x02), 1);
    ASSERT_EQ(count_packet_id(out, out_len, 0x01), 1);
    ASSERT_EQ(count_packet_id(out, out_len, 0x21), 1);

    mc_server_init(&server);
    mc_ringbuf_init(&tx, tx_storage, sizeof(tx_storage));
    ASSERT_TRUE(mc_server_receive(&server, handshake_status, sizeof(handshake_status), &tx));
    ASSERT_EQ(server.state, MC_CONN_STATUS);
    ASSERT_TRUE(mc_server_take_tx_reset(&server));
    ASSERT_TRUE(!mc_server_take_tx_reset(&server));
    ASSERT_TRUE(mc_server_receive(&server, status_request, sizeof(status_request), &tx));
    ASSERT_TRUE(mc_ringbuf_len(&tx) > 20u);

    ASSERT_TRUE(!mc_server_receive(&server, legacy_reset_magic, sizeof(legacy_reset_magic), &tx));
    ASSERT_EQ(server.state, MC_CONN_STATUS);
    ASSERT_TRUE(mc_ringbuf_len(&tx) > 20u);
    ASSERT_TRUE(!mc_server_take_tx_reset(&server));
    ASSERT_EQ(server.rx_accum_len, 0u);
    out_len = drain(&tx, out, sizeof(out));
    ASSERT_TRUE(out_len > 20u);
    ASSERT_TRUE(mc_server_receive(&server, status_ping, sizeof(status_ping), &tx));
    out_len = drain(&tx, out, sizeof(out));
    ASSERT_EQ(out_len, 10u);
    ASSERT_TRUE(packet_id_at(out, out_len, &packet_id));
    ASSERT_EQ(packet_id, 0x01);

    mc_server_init(&server);
    mc_ringbuf_init(&tx, tx_storage, sizeof(tx_storage));
    ASSERT_TRUE(mc_server_receive(&server, handshake_login, sizeof(handshake_login), &tx));
    ASSERT_EQ(server.state, MC_CONN_LOGIN);
    ASSERT_TRUE(mc_server_receive(&server, login_start, sizeof(login_start), &tx));
    ASSERT_EQ(server.state, MC_CONN_PLAY);

    out_len = drain(&tx, out, sizeof(out));
    ASSERT_TRUE(out_len > 20u);
    ASSERT_TRUE(packet_id_at(out, out_len, &packet_id));
    ASSERT_EQ(packet_id, 0x02);
    ASSERT_EQ(count_packet_id(out, out_len, 0x00), 0);
    ASSERT_EQ(count_packet_id(out, out_len, 0x02), 1);

    ASSERT_TRUE(mc_server_tick(&server, &tx));
    ASSERT_TRUE(mc_server_tick(&server, &tx));
    out_len = drain(&tx, out, sizeof(out));
    ASSERT_EQ(count_packet_id(out, out_len, 0x01), 1);
    ASSERT_EQ(count_packet_id(out, out_len, 0x21), 1);

    mc_server_init(&server);
    mc_ringbuf_init(&tx, tx_storage, sizeof(tx_storage));
    ASSERT_TRUE(mc_server_receive(&server, handshake_status, sizeof(handshake_status), &tx));
    ASSERT_EQ(server.state, MC_CONN_STATUS);
    ASSERT_TRUE(mc_server_take_tx_reset(&server));
    ASSERT_TRUE(!mc_server_take_tx_reset(&server));
    ASSERT_TRUE(mc_server_receive(&server, status_request, sizeof(status_request), &tx));
    ASSERT_TRUE(mc_ringbuf_len(&tx) > 20u);

    ASSERT_TRUE(mc_server_receive(&server,
                                  framed_payload_with_legacy_reset_magic,
                                  sizeof(framed_payload_with_legacy_reset_magic),
                                  &tx));
    ASSERT_EQ(server.state, MC_CONN_STATUS);
    ASSERT_TRUE(mc_ringbuf_len(&tx) > 20u);
    ASSERT_TRUE(!mc_server_take_tx_reset(&server));
    ASSERT_EQ(server.rx_accum_len, 0u);

    return 0;
}
