#include <stdint.h>
#include "mc_config.h"
#include "mc_packet.h"
#include "mc_ringbuf.h"
#include "mc_server.h"
#include "mc_varint.h"

#define ASSERT_TRUE(expr) do { if (!(expr)) return 1; } while (0)
#define ASSERT_EQ(a, b) do { if ((a) != (b)) return 1; } while (0)

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

int test_integration(void)
{
    mc_server_t server;
    uint8_t tx_storage[MC_TX_RING_CAP];
    uint8_t out[MC_TX_RING_CAP];
    mc_ringbuf_t tx;
    size_t out_len;

    const uint8_t handshake_login[] = {
        0x0f, 0x00, 0x2f, 0x09, '1','2','7','.','0','.','0','.','1', 0x63, 0xdd, 0x02
    };
    const uint8_t login_start[] = {
        0x09, 0x00, 0x07, 'p','l','a','y','e','r','1'
    };

    mc_ringbuf_init(&tx, tx_storage, sizeof(tx_storage));
    mc_server_init(&server);

    ASSERT_TRUE(mc_server_receive(&server, handshake_login, sizeof(handshake_login), &tx));
    ASSERT_EQ(server.state, MC_CONN_LOGIN);
    ASSERT_TRUE(mc_server_receive(&server, login_start, sizeof(login_start), &tx));
    ASSERT_EQ(server.state, MC_CONN_PLAY);

    ASSERT_TRUE(mc_server_tick(&server, &tx));
    ASSERT_TRUE(mc_server_tick(&server, &tx));

    out_len = mc_ringbuf_read(&tx, out, sizeof(out));
    ASSERT_TRUE(out_len > 12000u);
    ASSERT_EQ(count_packet_id(out, out_len, 0x02), 1);
    ASSERT_EQ(count_packet_id(out, out_len, 0x01), 1);
    ASSERT_EQ(count_packet_id(out, out_len, 0x21), 1);
    return 0;
}
