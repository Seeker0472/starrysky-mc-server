#include "mc_server.h"
#include "mc_config.h"
#include "mc_packet.h"
#include "mc_varint.h"
#include "mc_world.h"
#include <string.h>

typedef enum {
    MC_FRAME_NEED_MORE = 0,
    MC_FRAME_READY = 1,
    MC_FRAME_INVALID = 2
} mc_frame_status_t;

enum {
    MC_BOOTSTRAP_JOIN_GAME = 0,
    MC_BOOTSTRAP_SPAWN_POSITION,
    MC_BOOTSTRAP_TIME,
    MC_BOOTSTRAP_HEALTH,
    MC_BOOTSTRAP_POSITION,
    MC_BOOTSTRAP_CHUNKS,
    MC_BOOTSTRAP_DONE
};

static const uint8_t bridge_reset_magic[] = {
    0xff, 0x00, 0xff, 'M', 'C', 'U', 'R', 'S', 'T', 0x7e
};

static int read_varint_body(const uint8_t *body, size_t body_len, size_t *pos, int32_t *value)
{
    size_t used = 0;
    if (*pos >= body_len || !mc_varint_decode(body + *pos, body_len - *pos, value, &used)) {
        return 0;
    }
    *pos += used;
    return 1;
}

static int read_string_body(const uint8_t *body, size_t body_len, size_t *pos, char *dst, size_t dst_cap)
{
    int32_t len = 0;
    if (!read_varint_body(body, body_len, pos, &len) || len < 0) {
        return 0;
    }
    if ((size_t)len >= dst_cap || *pos + (size_t)len > body_len) {
        return 0;
    }
    memcpy(dst, body + *pos, (size_t)len);
    dst[len] = '\0';
    *pos += (size_t)len;
    return 1;
}

int mc_server_queue_packet(mc_ringbuf_t *tx, const uint8_t *body, size_t body_len)
{
    uint8_t prefix[5];
    size_t prefix_len = 0;
    size_t free_len = mc_ringbuf_free(tx);
    if (body_len > MC_MAX_PACKET_BODY) {
        return 0;
    }
    prefix_len = mc_varint_encode((int32_t)body_len, prefix, sizeof(prefix));
    if (prefix_len == 0u || free_len < prefix_len || free_len - prefix_len < body_len) {
        return 0;
    }
    return mc_ringbuf_write(tx, prefix, prefix_len) == prefix_len &&
           mc_ringbuf_write(tx, body, body_len) == body_len;
}

static int queue_status_response(mc_ringbuf_t *tx)
{
    static const char json[] =
        "{\"version\":{\"name\":\"1.8.x\",\"protocol\":47},"
        "\"players\":{\"max\":1,\"online\":0},"
        "\"description\":{\"text\":\"MC UART\"}}";
    uint8_t body[512];
    mc_writer_t w;
    mc_writer_init(&w, body, sizeof(body));
    return mc_write_varint(&w, 0x00) &&
           mc_write_string(&w, json) &&
           mc_server_queue_packet(tx, body, w.len);
}

static int queue_pong(mc_ringbuf_t *tx, const uint8_t *payload)
{
    uint8_t body[16];
    mc_writer_t w;
    mc_writer_init(&w, body, sizeof(body));
    return mc_write_varint(&w, 0x01) &&
           mc_write_bytes(&w, payload, 8u) &&
           mc_server_queue_packet(tx, body, w.len);
}

static int queue_login_success(mc_ringbuf_t *tx, const char *username)
{
    uint8_t body[128];
    mc_writer_t w;
    mc_writer_init(&w, body, sizeof(body));
    return mc_write_varint(&w, 0x02) &&
           mc_write_string(&w, "00000000-0000-0000-0000-000000000001") &&
           mc_write_string(&w, username) &&
           mc_server_queue_packet(tx, body, w.len);
}

static int queue_join_game(mc_ringbuf_t *tx)
{
    uint8_t body[128];
    mc_writer_t w;
    mc_writer_init(&w, body, sizeof(body));
    return mc_write_varint(&w, 0x01) &&
           mc_write_i32(&w, MC_ENTITY_ID_SELF) &&
           mc_write_u8(&w, 1u) &&
           mc_write_i8(&w, MC_DIMENSION_OVERWORLD) &&
           mc_write_u8(&w, 0u) &&
           mc_write_u8(&w, 1u) &&
           mc_write_string(&w, "flat") &&
           mc_write_bool(&w, 0) &&
           mc_server_queue_packet(tx, body, w.len);
}

static int queue_spawn_position(mc_ringbuf_t *tx)
{
    uint8_t body[32];
    mc_writer_t w;
    mc_writer_init(&w, body, sizeof(body));
    return mc_write_varint(&w, 0x05) &&
           mc_write_i64(&w, 0) &&
           mc_server_queue_packet(tx, body, w.len);
}

static int queue_time(mc_ringbuf_t *tx)
{
    uint8_t body[32];
    mc_writer_t w;
    mc_writer_init(&w, body, sizeof(body));
    return mc_write_varint(&w, 0x03) &&
           mc_write_i64(&w, 0) &&
           mc_write_i64(&w, 6000) &&
           mc_server_queue_packet(tx, body, w.len);
}

static int queue_health(mc_ringbuf_t *tx)
{
    uint8_t body[32];
    mc_writer_t w;
    mc_writer_init(&w, body, sizeof(body));
    return mc_write_varint(&w, 0x06) &&
           mc_write_f32(&w, 20.0f) &&
           mc_write_varint(&w, 20) &&
           mc_write_f32(&w, 5.0f) &&
           mc_server_queue_packet(tx, body, w.len);
}

static int queue_position(mc_ringbuf_t *tx)
{
    uint8_t body[64];
    mc_writer_t w;
    mc_writer_init(&w, body, sizeof(body));
    return mc_write_varint(&w, 0x08) &&
           mc_write_f64(&w, 0.5) &&
           mc_write_f64(&w, 6.0) &&
           mc_write_f64(&w, 0.5) &&
           mc_write_f32(&w, 0.0f) &&
           mc_write_f32(&w, 0.0f) &&
           mc_write_i8(&w, 0) &&
           mc_server_queue_packet(tx, body, w.len);
}

void mc_server_init(mc_server_t *server)
{
    memset(server, 0, sizeof(*server));
    server->state = MC_CONN_HANDSHAKE;
}

void mc_server_set_trace(mc_server_t *server, mc_trace_sink_t sink, void *user)
{
    server->trace_sink = sink;
    server->trace_user = user;
}

static void emit_trace(mc_server_t *server,
                       mc_trace_event_type_t type,
                       int32_t value0,
                       int32_t value1,
                       const char *text,
                       size_t text_len)
{
    if (server->trace_sink != 0) {
        mc_trace_event_t event;
        event.type = type;
        event.value0 = value0;
        event.value1 = value1;
        event.text = text;
        event.text_len = text_len;
        server->trace_sink(server->trace_user, &event);
    }
}

static void reset_logical_session(mc_server_t *server)
{
    memset(server->username, 0, sizeof(server->username));
    server->state = MC_CONN_HANDSHAKE;
    server->play_bootstrap_sent = 0;
    server->play_bootstrap_stage = 0;
    server->play_bootstrap_chunk_index = 0;
    server->tx_reset_requested = 1;
}

static int rx_accum_ends_with_reset_magic(const mc_server_t *server)
{
    size_t magic_len = sizeof(bridge_reset_magic);
    if (server->rx_accum_len < magic_len) {
        return 0;
    }
    for (size_t i = 0; i < magic_len; i++) {
        if (server->rx_accum[server->rx_accum_len - magic_len + i] != bridge_reset_magic[i]) {
            return 0;
        }
    }
    return 1;
}

typedef enum {
    MC_HANDSHAKE_NO_MATCH = 0,
    MC_HANDSHAKE_ACCEPTED = 1,
    MC_HANDSHAKE_INVALID = 2
} mc_handshake_result_t;

static mc_handshake_result_t try_handle_handshake(mc_server_t *server, const mc_packet_t *packet)
{
    size_t pos = 0;
    int32_t packet_id = 0;
    int32_t proto = 0;
    char host[256];
    int32_t next_state = 0;

    if (!read_varint_body(packet->body, packet->body_len, &pos, &packet_id)) {
        return MC_HANDSHAKE_INVALID;
    }
    if (packet_id != 0x00) {
        return MC_HANDSHAKE_NO_MATCH;
    }
    if (!read_varint_body(packet->body, packet->body_len, &pos, &proto)) return MC_HANDSHAKE_INVALID;
    if (!read_string_body(packet->body, packet->body_len, &pos, host, sizeof(host))) return MC_HANDSHAKE_INVALID;
    if (pos + 2u > packet->body_len) return MC_HANDSHAKE_INVALID;
    pos += 2u;
    if (!read_varint_body(packet->body, packet->body_len, &pos, &next_state)) return MC_HANDSHAKE_INVALID;
    if (pos != packet->body_len) return MC_HANDSHAKE_INVALID;

    reset_logical_session(server);
    if (next_state == 1) {
        server->state = MC_CONN_STATUS;
    } else if (next_state == 2) {
        server->state = MC_CONN_LOGIN;
    } else {
        return MC_HANDSHAKE_INVALID;
    }
    emit_trace(server, MC_TRACE_HANDSHAKE, next_state, proto, host, strlen(host));
    return MC_HANDSHAKE_ACCEPTED;
}

static int handle_packet(mc_server_t *server, const mc_packet_t *packet, mc_ringbuf_t *tx)
{
    size_t pos = 0;
    int32_t packet_id = 0;

    if (!read_varint_body(packet->body, packet->body_len, &pos, &packet_id)) {
        return 0;
    }

    if (server->state == MC_CONN_HANDSHAKE && packet_id == 0x00) {
        if (try_handle_handshake(server, packet) != MC_HANDSHAKE_ACCEPTED) {
            return 0;
        }
        mc_ringbuf_drop(tx, mc_ringbuf_len(tx));
        return 1;
    }

    if (server->state != MC_CONN_PLAY && packet_id == 0x00) {
        mc_handshake_result_t handshake = try_handle_handshake(server, packet);
        if (handshake == MC_HANDSHAKE_ACCEPTED) {
            mc_ringbuf_drop(tx, mc_ringbuf_len(tx));
            return 1;
        }
        if (handshake == MC_HANDSHAKE_INVALID && server->state != MC_CONN_STATUS && server->state != MC_CONN_LOGIN) {
            return 0;
        }
    }

    if (server->state == MC_CONN_STATUS && packet_id == 0x00) {
        emit_trace(server, MC_TRACE_STATUS_REQUEST, 0, 0, 0, 0u);
        return queue_status_response(tx);
    }

    if (server->state == MC_CONN_STATUS && packet_id == 0x01 && packet->body_len >= pos + 8u) {
        emit_trace(server, MC_TRACE_STATUS_PING, 0, 0, 0, 0u);
        return queue_pong(tx, packet->body + pos);
    }

    if (server->state == MC_CONN_LOGIN && packet_id == 0x00) {
        char username[MC_MAX_USERNAME + 1u];
        if (!read_string_body(packet->body, packet->body_len, &pos, username, sizeof(username))) {
            return 0;
        }
        if (username[0] == '\0') {
            return 0;
        }
        emit_trace(server, MC_TRACE_LOGIN_START, 0, 0, username, strlen(username));
        if (!queue_login_success(tx, username)) {
            return 0;
        }
        memcpy(server->username, username, sizeof(username));
        server->state = MC_CONN_PLAY;
        emit_trace(server, MC_TRACE_PLAY_ENTER, 0, 0, server->username, strlen(server->username));
        return 1;
    }

    return 1;
}

static mc_frame_status_t classify_frame(const uint8_t *src, size_t src_len, size_t *frame_len)
{
    int32_t body_len = 0;
    size_t prefix_len = 0;

    for (size_t i = 0; i < src_len && i < 5u; i++) {
        if ((src[i] & 0x80u) == 0u) {
            if (!mc_varint_decode(src, i + 1u, &body_len, &prefix_len)) {
                return MC_FRAME_INVALID;
            }
            if (body_len < 0 || (size_t)body_len > MC_MAX_PACKET_BODY) {
                return MC_FRAME_INVALID;
            }
            *frame_len = prefix_len + (size_t)body_len;
            return src_len >= *frame_len ? MC_FRAME_READY : MC_FRAME_NEED_MORE;
        }
    }

    return src_len >= 5u ? MC_FRAME_INVALID : MC_FRAME_NEED_MORE;
}

int mc_server_receive(mc_server_t *server, const uint8_t *bytes, size_t len, mc_ringbuf_t *tx)
{
    for (size_t i = 0; i < len; i++) {
        if (server->rx_accum_len == sizeof(server->rx_accum)) {
            server->rx_accum_len = 0;
            return 0;
        }
        server->rx_accum[server->rx_accum_len++] = bytes[i];
        if (rx_accum_ends_with_reset_magic(server)) {
            reset_logical_session(server);
            emit_trace(server, MC_TRACE_BRIDGE_RESET, 0, 0, 0, 0u);
            mc_ringbuf_drop(tx, mc_ringbuf_len(tx));
            server->rx_accum_len = 0;
        }
    }

    for (;;) {
        mc_packet_t packet;
        size_t frame_len = 0;
        mc_frame_status_t status = classify_frame(server->rx_accum, server->rx_accum_len, &frame_len);
        if (status == MC_FRAME_NEED_MORE) {
            break;
        }
        if (status == MC_FRAME_INVALID) {
            server->rx_accum_len = 0;
            return 0;
        }
        emit_trace(server, MC_TRACE_FRAME_READY, (int32_t)frame_len, (int32_t)server->state, 0, 0u);
        if (!mc_packet_try_read(server->rx_accum, server->rx_accum_len, &packet)) {
            server->rx_accum_len = 0;
            return 0;
        }
        if (!handle_packet(server, &packet, tx)) {
            server->rx_accum_len = 0;
            return 0;
        }
        memmove(server->rx_accum,
                server->rx_accum + packet.frame_len,
                server->rx_accum_len - packet.frame_len);
        server->rx_accum_len -= packet.frame_len;
    }

    return 1;
}

int mc_server_take_tx_reset(mc_server_t *server)
{
    int requested = server->tx_reset_requested != 0u;
    server->tx_reset_requested = 0;
    return requested;
}

int mc_server_tick(mc_server_t *server, mc_ringbuf_t *tx)
{
    server->ticks++;
    if (server->state == MC_CONN_PLAY && !server->play_bootstrap_sent) {
        while (server->play_bootstrap_stage < MC_BOOTSTRAP_DONE) {
            int queued = 0;
            int advance_stage = 1;
            switch (server->play_bootstrap_stage) {
            case MC_BOOTSTRAP_JOIN_GAME:
                queued = queue_join_game(tx);
                break;
            case MC_BOOTSTRAP_SPAWN_POSITION:
                queued = queue_spawn_position(tx);
                break;
            case MC_BOOTSTRAP_TIME:
                queued = queue_time(tx);
                break;
            case MC_BOOTSTRAP_HEALTH:
                queued = queue_health(tx);
                break;
            case MC_BOOTSTRAP_POSITION:
                queued = queue_position(tx);
                break;
            case MC_BOOTSTRAP_CHUNKS:
                queued = mc_world_queue_spawn_chunk(tx, server->play_bootstrap_chunk_index);
                if (queued) {
                    server->play_bootstrap_chunk_index++;
                    if ((size_t)server->play_bootstrap_chunk_index < mc_world_spawn_chunk_count()) {
                        advance_stage = 0;
                    }
                }
                break;
            default:
                queued = 1;
                break;
            }
            if (!queued) {
                emit_trace(server,
                           MC_TRACE_QUEUE_FULL,
                           server->play_bootstrap_stage,
                           server->play_bootstrap_chunk_index,
                           0,
                           0u);
                return 0;
            }
            if (advance_stage) {
                emit_trace(server,
                           MC_TRACE_BOOTSTRAP_STAGE,
                           server->play_bootstrap_stage,
                           server->play_bootstrap_chunk_index,
                           0,
                           0u);
                server->play_bootstrap_stage++;
            }
            if (server->play_bootstrap_stage == MC_BOOTSTRAP_CHUNKS) {
                break;
            }
        }
        if (server->play_bootstrap_stage >= MC_BOOTSTRAP_DONE) {
            server->play_bootstrap_sent = 1;
            emit_trace(server,
                       MC_TRACE_BOOTSTRAP_DONE,
                       server->play_bootstrap_stage,
                       server->play_bootstrap_chunk_index,
                       0,
                       0u);
        }
    }
    return 1;
}
