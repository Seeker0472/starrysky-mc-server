#include <stdint.h>
#include <string.h>
#include "mc_config.h"
#include "mc_packet.h"
#include "mc_ringbuf.h"
#include "mc_server.h"
#include "mc_world.h"

#define ASSERT_TRUE(expr) do { if (!(expr)) return 1; } while (0)
#define ASSERT_EQ(a, b) do { if ((a) != (b)) return 1; } while (0)

typedef struct {
    int handshake_login;
    int login_start;
    int play_enter;
    int bootstrap_done;
    int keepalive_send;
    int keepalive_ack;
    int play_unhandled;
    int32_t keepalive_id;
    int32_t unhandled_packet_id;
    int32_t unhandled_body_len;
    int32_t keepalive_tx_len;
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
    if (event->type == MC_TRACE_BOOTSTRAP_DONE) {
        counts->bootstrap_done++;
    }
    if (event->type == MC_TRACE_KEEPALIVE_SEND) {
        counts->keepalive_send++;
        counts->keepalive_id = event->value0;
        counts->keepalive_tx_len = (int32_t)event->text_len;
    }
    if (event->type == MC_TRACE_KEEPALIVE_ACK) {
        counts->keepalive_ack++;
        counts->keepalive_id = event->value0;
    }
    if (event->type == MC_TRACE_PLAY_UNHANDLED) {
        counts->play_unhandled++;
        counts->unhandled_packet_id = event->value0;
        counts->unhandled_body_len = event->value1;
    }
}

static void drain_ring(mc_ringbuf_t *tx)
{
    uint8_t out[MC_TX_RING_CAP];
    (void)mc_ringbuf_read(tx, out, sizeof(out));
}

static size_t wrap_serverbound_body(const uint8_t *body, size_t body_len, uint8_t *dst, size_t dst_cap)
{
#if MC_PROTOCOL_COMPRESSION_ENABLE
    return mc_packet_wrap_compressed_plain(body, body_len, dst, dst_cap);
#else
    return mc_packet_wrap(body, body_len, dst, dst_cap);
#endif
}

static int test_server_keepalive_trace(void);
static int test_server_keepalive_diagnostics_trace(void);

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
    const uint8_t legacy_reset_magic[] = {
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

    ASSERT_TRUE(!mc_server_receive(&server, legacy_reset_magic, sizeof(legacy_reset_magic), &tx));

    ASSERT_EQ(counts.handshake_login, 1);
    ASSERT_EQ(counts.login_start, 1);
    ASSERT_EQ(counts.play_enter, 1);
    ASSERT_EQ(counts.bootstrap_done, 1);
    ASSERT_TRUE(!test_server_keepalive_trace());
    ASSERT_TRUE(!test_server_keepalive_diagnostics_trace());

    return 0;
}

static int test_server_keepalive_trace(void)
{
    mc_server_t server;
    uint8_t tx_storage[MC_TX_RING_CAP];
    mc_ringbuf_t tx;
    trace_counts_t counts;
    uint8_t response_body[8];
    uint8_t response_frame[16];
    size_t response_frame_len;
    mc_writer_t w;

    const uint8_t handshake_login[] = {
        0x0f, 0x00, 0x2f, 0x09, '1','2','7','.','0','.','0','.','1', 0x63, 0xdd, 0x02
    };
    const uint8_t login_start[] = {
        0x09, 0x00, 0x07, 'p','l','a','y','e','r','1'
    };

    memset(&counts, 0, sizeof(counts));
    mc_ringbuf_init(&tx, tx_storage, sizeof(tx_storage));
    mc_server_init(&server);
    mc_server_set_trace(&server, trace_sink, &counts);

    ASSERT_TRUE(mc_server_receive(&server, handshake_login, sizeof(handshake_login), &tx));
    ASSERT_TRUE(mc_server_receive(&server, login_start, sizeof(login_start), &tx));
    drain_ring(&tx);
    for (size_t i = 0; i <= mc_world_spawn_chunk_count(); i++) {
        ASSERT_TRUE(mc_server_tick_at(&server, &tx, 1u));
        drain_ring(&tx);
    }

    ASSERT_EQ(counts.keepalive_send, 1);
    ASSERT_EQ(counts.keepalive_id, 1);
    ASSERT_TRUE(counts.keepalive_tx_len > 0);
    drain_ring(&tx);

    ASSERT_TRUE(mc_server_tick_at(&server, &tx, MC_KEEPALIVE_INTERVAL_TICKS + 1u));
    ASSERT_EQ(counts.keepalive_send, 2);
    ASSERT_EQ(counts.keepalive_id, 2);

    mc_writer_init(&w, response_body, sizeof(response_body));
    ASSERT_TRUE(mc_write_varint(&w, 0x00));
    ASSERT_TRUE(mc_write_varint(&w, 1));
    response_frame_len = wrap_serverbound_body(response_body,
                                               w.len,
                                               response_frame,
                                               sizeof(response_frame));
    ASSERT_TRUE(response_frame_len > 0u);
    ASSERT_TRUE(mc_server_receive(&server, response_frame, response_frame_len, &tx));
    ASSERT_EQ(counts.keepalive_ack, 1);
    ASSERT_EQ(counts.keepalive_id, 1);

    return 0;
}

static int test_server_keepalive_diagnostics_trace(void)
{
    mc_server_t server;
    uint8_t tx_storage[MC_TX_RING_CAP];
    mc_ringbuf_t tx;
    trace_counts_t counts;
    uint8_t body[16];
    uint8_t frame[32];
    size_t frame_len;
    mc_writer_t w;

    const uint8_t handshake_login[] = {
        0x0f, 0x00, 0x2f, 0x09, '1','2','7','.','0','.','0','.','1', 0x63, 0xdd, 0x02
    };
    const uint8_t login_start[] = {
        0x09, 0x00, 0x07, 'p','l','a','y','e','r','1'
    };

    memset(&counts, 0, sizeof(counts));
    mc_ringbuf_init(&tx, tx_storage, sizeof(tx_storage));
    mc_server_init(&server);
    mc_server_set_trace(&server, trace_sink, &counts);

    ASSERT_TRUE(mc_server_receive(&server, handshake_login, sizeof(handshake_login), &tx));
    ASSERT_TRUE(mc_server_receive(&server, login_start, sizeof(login_start), &tx));
    drain_ring(&tx);
    for (size_t i = 0; i <= mc_world_spawn_chunk_count(); i++) {
        ASSERT_TRUE(mc_server_tick_at(&server, &tx, 1u));
        drain_ring(&tx);
    }

    ASSERT_EQ(counts.keepalive_send, 1);
    ASSERT_EQ(counts.keepalive_id, 1);
    ASSERT_EQ(server.keepalive_pending, 1u);
    drain_ring(&tx);

    mc_writer_init(&w, body, sizeof(body));
    ASSERT_TRUE(mc_write_varint(&w, 0x00));
    ASSERT_TRUE(mc_write_varint(&w, 99));
    frame_len = wrap_serverbound_body(body, w.len, frame, sizeof(frame));
    ASSERT_TRUE(frame_len > 0u);
    ASSERT_TRUE(mc_server_receive(&server, frame, frame_len, &tx));
    ASSERT_EQ(counts.keepalive_ack, 1);
    ASSERT_EQ(counts.keepalive_id, 99);
    ASSERT_EQ(server.keepalive_pending, 0u);

    mc_writer_init(&w, body, sizeof(body));
    ASSERT_TRUE(mc_write_varint(&w, 0x15));
    frame_len = wrap_serverbound_body(body, w.len, frame, sizeof(frame));
    ASSERT_TRUE(frame_len > 0u);
    ASSERT_TRUE(mc_server_receive(&server, frame, frame_len, &tx));
    ASSERT_EQ(counts.play_unhandled, 1);
    ASSERT_EQ(counts.unhandled_packet_id, 0x15);
    ASSERT_EQ(counts.unhandled_body_len, (int32_t)w.len);

    return 0;
}
