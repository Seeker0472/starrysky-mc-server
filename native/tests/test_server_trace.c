#include <stdint.h>
#include <string.h>
#include "mc_config.h"
#include "mc_ringbuf.h"
#include "mc_server.h"
#include "mc_world.h"

#define ASSERT_TRUE(expr) do { if (!(expr)) return 1; } while (0)
#define ASSERT_EQ(a, b) do { if ((a) != (b)) return 1; } while (0)

typedef struct {
    int handshake_login;
    int login_start;
    int play_enter;
    int bridge_reset;
    int bootstrap_done;
} trace_counts_t;

static void trace_sink(void *user, const mc_trace_event_t *event)
{
    trace_counts_t *counts = (trace_counts_t *)user;

    if (event->type == MC_TRACE_HANDSHAKE && event->value0 == 2) {
        counts->handshake_login++;
    }
    if (event->type == MC_TRACE_LOGIN_START &&
        event->text_len == 7u &&
        memcmp(event->text, "player1", 7u) == 0) {
        counts->login_start++;
    }
    if (event->type == MC_TRACE_PLAY_ENTER) {
        counts->play_enter++;
    }
    if (event->type == MC_TRACE_BRIDGE_RESET) {
        counts->bridge_reset++;
    }
    if (event->type == MC_TRACE_BOOTSTRAP_DONE) {
        counts->bootstrap_done++;
    }
}

static void drain_ring(mc_ringbuf_t *tx)
{
    uint8_t out[MC_TX_RING_CAP];
    (void)mc_ringbuf_read(tx, out, sizeof(out));
}

int test_server_trace(void)
{
    mc_server_t server;
    uint8_t tx_storage[MC_TX_RING_CAP];
    mc_ringbuf_t tx;
    trace_counts_t counts;

    const uint8_t handshake_login[] = {
        0x0f, 0x00, 0x2f, 0x09, '1','2','7','.','0','.','0','.','1', 0x63, 0xdd, 0x02
    };
    const uint8_t login_start[] = {
        0x09, 0x00, 0x07, 'p','l','a','y','e','r','1'
    };
    const uint8_t bridge_reset[] = {
        0xff, 0x00, 0xff, 'M', 'C', 'U', 'R', 'S', 'T', 0x7e
    };

    memset(&counts, 0, sizeof(counts));
    mc_ringbuf_init(&tx, tx_storage, sizeof(tx_storage));
    mc_server_init(&server);
    mc_server_set_trace(&server, trace_sink, &counts);

    ASSERT_TRUE(mc_server_receive(&server, handshake_login, sizeof(handshake_login), &tx));
    ASSERT_TRUE(mc_server_receive(&server, login_start, sizeof(login_start), &tx));

    ASSERT_TRUE(mc_server_tick(&server, &tx));
    drain_ring(&tx);

    for (size_t i = 0; i < mc_world_spawn_chunk_count(); i++) {
        ASSERT_TRUE(mc_server_tick(&server, &tx));
        drain_ring(&tx);
    }

    ASSERT_TRUE(mc_server_receive(&server, bridge_reset, sizeof(bridge_reset), &tx));

    ASSERT_EQ(counts.handshake_login, 1);
    ASSERT_EQ(counts.login_start, 1);
    ASSERT_EQ(counts.play_enter, 1);
    ASSERT_EQ(counts.bridge_reset, 1);
    ASSERT_EQ(counts.bootstrap_done, 1);

    return 0;
}
