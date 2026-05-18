#ifndef MC_SERVER_H
#define MC_SERVER_H

#include <stddef.h>
#include <stdint.h>
#include "mc_config.h"
#include "mc_ringbuf.h"

typedef enum {
    MC_CONN_HANDSHAKE = 0,
    MC_CONN_STATUS = 1,
    MC_CONN_LOGIN = 2,
    MC_CONN_PLAY = 3
} mc_conn_state_t;

typedef enum {
    MC_TRACE_FRAME_READY = 0,
    MC_TRACE_HANDSHAKE = 1,
    MC_TRACE_STATUS_REQUEST = 2,
    MC_TRACE_STATUS_PING = 3,
    MC_TRACE_LOGIN_START = 4,
    MC_TRACE_PLAY_ENTER = 5,
    MC_TRACE_BOOTSTRAP_STAGE = 7,
    MC_TRACE_BOOTSTRAP_DONE = 8,
    MC_TRACE_QUEUE_FULL = 9
} mc_trace_event_type_t;

typedef struct {
    mc_trace_event_type_t type;
    int32_t value0;
    int32_t value1;
    const char *text;
    size_t text_len;
} mc_trace_event_t;

/*
 * Trace callbacks are invoked synchronously from the server call that emits
 * them. The event object and event->text, when non-null, are valid only until
 * the callback returns. Sinks that need trace data later must copy it before
 * returning.
 *
 * MC_TRACE_STATUS_REQUEST, MC_TRACE_STATUS_PING, and MC_TRACE_LOGIN_START mark
 * packet handling attempts and are emitted before the associated TX queue
 * operation is known to have succeeded. MC_TRACE_PLAY_ENTER and
 * MC_TRACE_BOOTSTRAP_DONE represent completed state transitions.
 */
typedef void (*mc_trace_sink_t)(void *user, const mc_trace_event_t *event);

typedef struct {
    mc_conn_state_t state;
    char username[MC_MAX_USERNAME + 1u];
    uint32_t ticks;
    int play_bootstrap_sent;
    uint8_t rx_accum[MC_MAX_PACKET_BODY + 8u];
    size_t rx_accum_len;
    uint8_t play_bootstrap_stage;
    uint8_t play_bootstrap_chunk_index;
    uint8_t tx_reset_requested;
    mc_trace_sink_t trace_sink;
    void *trace_user;
} mc_server_t;

void mc_server_init(mc_server_t *server);
void mc_server_set_trace(mc_server_t *server, mc_trace_sink_t sink, void *user);
int mc_server_receive(mc_server_t *server, const uint8_t *bytes, size_t len, mc_ringbuf_t *tx);
int mc_server_tick(mc_server_t *server, mc_ringbuf_t *tx);
int mc_server_queue_packet(mc_ringbuf_t *tx, const uint8_t *body, size_t body_len);
int mc_server_take_tx_reset(mc_server_t *server);

#endif
