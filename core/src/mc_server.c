#include "mc_server.h"
#include "mc_config.h"
#include "mc_packet.h"
#include "mc_varint.h"
#include "mc_world.h"
#if MC_USE_PSRAM_COMPRESSED_MAP
#include "mc_world_compressed.h"
#endif
#include <limits.h>
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

enum {
    MC_MAX_SERVERBOUND_PACKET_ID = 0x7f
};

#if MC_PROTOCOL_COMPRESSION_ENABLE && !MC_USE_PSRAM_COMPRESSED_MAP
static const int32_t server_spawn_chunks[][2] = {
    {0, 0},
    {1, 0},
    {-1, 0},
    {0, 1},
    {0, -1},
    {1, 1},
    {-1, -1},
    {1, -1},
    {-1, 1},
};

static uint8_t server_chunk_body[MC_CHUNK_SECTION_BYTES + 32u];
#endif

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

static int read_bool_body(const uint8_t *body, size_t body_len, size_t *pos, uint8_t *value)
{
    if (*pos >= body_len) {
        return 0;
    }
    *value = body[*pos] ? 1u : 0u;
    (*pos)++;
    return 1;
}

static int read_u32_body(const uint8_t *body, size_t body_len, size_t *pos, uint32_t *value)
{
    if (*pos > body_len || body_len - *pos < 4u) {
        return 0;
    }
    *value = ((uint32_t)body[*pos] << 24) |
             ((uint32_t)body[*pos + 1u] << 16) |
             ((uint32_t)body[*pos + 2u] << 8) |
             (uint32_t)body[*pos + 3u];
    *pos += 4u;
    return 1;
}

static int read_u64_body(const uint8_t *body, size_t body_len, size_t *pos, uint64_t *value)
{
    uint64_t v = 0u;
    if (*pos > body_len || body_len - *pos < 8u) {
        return 0;
    }
    for (size_t i = 0; i < 8u; i++) {
        v = (v << 8) | (uint64_t)body[*pos + i];
    }
    *value = v;
    *pos += 8u;
    return 1;
}

static int read_f32_body(const uint8_t *body, size_t body_len, size_t *pos, float *value)
{
    union { float f; uint32_t u; } u;
    if (!read_u32_body(body, body_len, pos, &u.u)) {
        return 0;
    }
    *value = u.f;
    return 1;
}

static int read_f64_body(const uint8_t *body, size_t body_len, size_t *pos, double *value)
{
    union { double d; uint64_t u; } u;
    if (!read_u64_body(body, body_len, pos, &u.u)) {
        return 0;
    }
    *value = u.d;
    return 1;
}

static uint32_t elapsed_ticks(uint32_t now_ticks, uint32_t then_ticks)
{
    return now_ticks - then_ticks;
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

#if MC_PROTOCOL_COMPRESSION_ENABLE
static int queue_packet_compressed_plain(mc_ringbuf_t *tx, const uint8_t *body, size_t body_len)
{
    uint8_t prefix[5];
    uint8_t zero = 0u;
    size_t inner_len = 1u + body_len;
    size_t prefix_len = 0;
    size_t free_len = mc_ringbuf_free(tx);

    if (body_len > MC_MAX_PACKET_BODY || inner_len > (size_t)INT32_MAX) {
        return 0;
    }
    prefix_len = mc_varint_encode((int32_t)inner_len, prefix, sizeof(prefix));
    if (prefix_len == 0u || free_len < prefix_len + inner_len) {
        return 0;
    }
    return mc_ringbuf_write(tx, prefix, prefix_len) == prefix_len &&
           mc_ringbuf_write(tx, &zero, 1u) == 1u &&
           mc_ringbuf_write(tx, body, body_len) == body_len;
}
#endif

static int queue_packet_auto(const mc_server_t *server, mc_ringbuf_t *tx, const uint8_t *body, size_t body_len)
{
#if MC_PROTOCOL_COMPRESSION_ENABLE
    if (server->compression_enabled) {
        return queue_packet_compressed_plain(tx, body, body_len);
    }
#else
    (void)server;
#endif
    return mc_server_queue_packet(tx, body, body_len);
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

static int build_login_success_body(uint8_t *body, size_t body_cap, const char *username, size_t *body_len)
{
    mc_writer_t w;
    mc_writer_init(&w, body, body_cap);
    if (!mc_write_varint(&w, 0x02) ||
        !mc_write_string(&w, "00000000-0000-0000-0000-000000000001") ||
        !mc_write_string(&w, username)) {
        return 0;
    }
    *body_len = w.len;
    return 1;
}

#if !MC_PROTOCOL_COMPRESSION_ENABLE
static int queue_login_success(mc_ringbuf_t *tx, const char *username)
{
    uint8_t body[128];
    size_t body_len = 0;
    return build_login_success_body(body, sizeof(body), username, &body_len) &&
           mc_server_queue_packet(tx, body, body_len);
}
#endif

#if MC_PROTOCOL_COMPRESSION_ENABLE
static int build_set_compression(uint8_t *body, size_t body_cap, int32_t threshold, size_t *body_len)
{
    mc_writer_t w;
    mc_writer_init(&w, body, body_cap);
    if (!mc_write_varint(&w, 0x03) ||
        !mc_write_varint(&w, threshold)) {
        return 0;
    }
    *body_len = w.len;
    return 1;
}
#endif

static int queue_join_game(const mc_server_t *server, mc_ringbuf_t *tx)
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
           queue_packet_auto(server, tx, body, w.len);
}

static int queue_spawn_position(const mc_server_t *server, mc_ringbuf_t *tx)
{
    uint8_t body[32];
    mc_writer_t w;
    mc_writer_init(&w, body, sizeof(body));
    return mc_write_varint(&w, 0x05) &&
           mc_write_i64(&w, 0) &&
           queue_packet_auto(server, tx, body, w.len);
}

static int queue_time(const mc_server_t *server, mc_ringbuf_t *tx)
{
    uint8_t body[32];
    mc_writer_t w;
    mc_writer_init(&w, body, sizeof(body));
    return mc_write_varint(&w, 0x03) &&
           mc_write_i64(&w, 0) &&
           mc_write_i64(&w, 6000) &&
           queue_packet_auto(server, tx, body, w.len);
}

static int queue_health(const mc_server_t *server, mc_ringbuf_t *tx)
{
    uint8_t body[32];
    mc_writer_t w;
    mc_writer_init(&w, body, sizeof(body));
    return mc_write_varint(&w, 0x06) &&
           mc_write_f32(&w, 20.0f) &&
           mc_write_varint(&w, 20) &&
           mc_write_f32(&w, 5.0f) &&
           queue_packet_auto(server, tx, body, w.len);
}

static int queue_position(const mc_server_t *server, mc_ringbuf_t *tx)
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
           queue_packet_auto(server, tx, body, w.len);
}

static int queue_keepalive(const mc_server_t *server, mc_ringbuf_t *tx, int32_t keepalive_id)
{
    uint8_t body[16];
    mc_writer_t w;
    mc_writer_init(&w, body, sizeof(body));
    return mc_write_varint(&w, 0x00) &&
           mc_write_varint(&w, keepalive_id) &&
           queue_packet_auto(server, tx, body, w.len);
}

static void reset_play_runtime_state(mc_server_t *server)
{
    server->last_keepalive_tick = 0u;
    server->keepalive_id = 0;
    server->keepalive_pending = 0u;
    server->player_x = 0.5;
    server->player_y = 6.0;
    server->player_z = 0.5;
    server->player_yaw = 0.0f;
    server->player_pitch = 0.0f;
    server->player_on_ground = 0u;
}

#if MC_PROTOCOL_COMPRESSION_ENABLE && !MC_USE_PSRAM_COMPRESSED_MAP
static int queue_spawn_chunk_auto(const mc_server_t *server, mc_ringbuf_t *tx, size_t index)
{
    size_t body_len = 0;

    if (!server->compression_enabled) {
        return mc_world_queue_spawn_chunk(tx, index);
    }
    if (index >= sizeof(server_spawn_chunks) / sizeof(server_spawn_chunks[0]) ||
        index >= mc_world_spawn_chunk_count()) {
        return 0;
    }
    if (!mc_world_build_chunk_body(server_spawn_chunks[index][0],
                                   server_spawn_chunks[index][1],
                                   server_chunk_body,
                                   sizeof(server_chunk_body),
                                   &body_len)) {
        return 0;
    }
    return queue_packet_auto(server, tx, server_chunk_body, body_len);
}
#endif

void mc_server_init(mc_server_t *server)
{
    memset(server, 0, sizeof(*server));
    server->state = MC_CONN_HANDSHAKE;
    server->compression_enabled = 0;
    server->compression_threshold = (int32_t)MC_COMPRESSION_THRESHOLD;
    reset_play_runtime_state(server);
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
    server->compression_enabled = 0;
    server->compression_threshold = (int32_t)MC_COMPRESSION_THRESHOLD;
    server->play_bootstrap_sent = 0;
    server->play_bootstrap_stage = 0;
    server->play_bootstrap_chunk_index = 0;
    server->tx_reset_requested = 1;
    reset_play_runtime_state(server);
}

static int tick_keepalive(mc_server_t *server, mc_ringbuf_t *tx, uint32_t now_ticks)
{
    int32_t keepalive_id = server->keepalive_id;

    if (server->state != MC_CONN_PLAY || !server->play_bootstrap_sent) {
        return 1;
    }
    if (server->last_keepalive_tick != 0u &&
        elapsed_ticks(now_ticks, server->last_keepalive_tick) < MC_KEEPALIVE_INTERVAL_TICKS) {
        return 1;
    }
    if (server->keepalive_id >= INT32_MAX) {
        keepalive_id = 1;
    } else {
        keepalive_id = server->keepalive_id + 1;
    }
    if (!queue_keepalive(server, tx, keepalive_id)) {
        return 0;
    }
    server->keepalive_id = keepalive_id;
    server->keepalive_pending = 1u;
    server->last_keepalive_tick = now_ticks;
    emit_trace(server,
               MC_TRACE_KEEPALIVE_SEND,
               keepalive_id,
               0,
               0,
               mc_ringbuf_len(tx));
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

static int handle_play_movement(mc_server_t *server,
                                int32_t packet_id,
                                const uint8_t *body,
                                size_t body_len,
                                size_t pos)
{
    double x = server->player_x;
    double y = server->player_y;
    double z = server->player_z;
    float yaw = server->player_yaw;
    float pitch = server->player_pitch;
    uint8_t on_ground = server->player_on_ground;

    switch (packet_id) {
    case 0x03:
        if (!read_bool_body(body, body_len, &pos, &on_ground)) {
            return 0;
        }
        break;
    case 0x04:
        if (!read_f64_body(body, body_len, &pos, &x) ||
            !read_f64_body(body, body_len, &pos, &y) ||
            !read_f64_body(body, body_len, &pos, &z) ||
            !read_bool_body(body, body_len, &pos, &on_ground)) {
            return 0;
        }
        break;
    case 0x05:
        if (!read_f32_body(body, body_len, &pos, &yaw) ||
            !read_f32_body(body, body_len, &pos, &pitch) ||
            !read_bool_body(body, body_len, &pos, &on_ground)) {
            return 0;
        }
        break;
    case 0x06:
        if (!read_f64_body(body, body_len, &pos, &x) ||
            !read_f64_body(body, body_len, &pos, &y) ||
            !read_f64_body(body, body_len, &pos, &z) ||
            !read_f32_body(body, body_len, &pos, &yaw) ||
            !read_f32_body(body, body_len, &pos, &pitch) ||
            !read_bool_body(body, body_len, &pos, &on_ground)) {
            return 0;
        }
        break;
    default:
        return 0;
    }

    if (pos != body_len) {
        return 0;
    }

    server->player_x = x;
    server->player_y = y;
    server->player_z = z;
    server->player_yaw = yaw;
    server->player_pitch = pitch;
    server->player_on_ground = on_ground;
    return 1;
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
#if MC_PROTOCOL_COMPRESSION_ENABLE
        {
            uint8_t compression_body[16];
            size_t compression_body_len = 0;
            uint8_t success_body[128];
            size_t success_body_len = 0;
            size_t needed;

            if (!build_set_compression(compression_body,
                                       sizeof(compression_body),
                                       server->compression_threshold,
                                       &compression_body_len) ||
                !build_login_success_body(success_body, sizeof(success_body), username, &success_body_len)) {
                return 0;
            }
            needed = mc_packet_frame_len(compression_body_len);
            needed += mc_packet_compressed_plain_frame_len(success_body_len);
            if (needed == 0u || mc_ringbuf_free(tx) < needed) {
                return 0;
            }
            if (!mc_server_queue_packet(tx, compression_body, compression_body_len)) {
                return 0;
            }
            server->compression_enabled = 1;
            if (!queue_packet_compressed_plain(tx, success_body, success_body_len)) {
                return 0;
            }
        }
#else
        if (!queue_login_success(tx, username)) {
            return 0;
        }
#endif
        memcpy(server->username, username, sizeof(username));
        server->state = MC_CONN_PLAY;
        emit_trace(server, MC_TRACE_PLAY_ENTER, 0, 0, server->username, strlen(server->username));
        return 1;
    }

    if (server->state == MC_CONN_PLAY && packet_id == 0x00) {
        int32_t keepalive_id = 0;
        if (!read_varint_body(packet->body, packet->body_len, &pos, &keepalive_id) ||
            pos != packet->body_len) {
            return 0;
        }
        if (server->keepalive_pending) {
            emit_trace(server,
                       MC_TRACE_KEEPALIVE_ACK,
                       keepalive_id,
                       0,
                       0,
                       0u);
            server->keepalive_pending = 0u;
        }
        return 1;
    }

    if (server->state == MC_CONN_PLAY &&
        (packet_id == 0x03 || packet_id == 0x04 || packet_id == 0x05 || packet_id == 0x06)) {
        if (!handle_play_movement(server, packet_id, packet->body, packet->body_len, pos)) {
            return 0;
        }
        return 1;
    }

    if (server->state == MC_CONN_PLAY) {
        emit_trace(server,
                   MC_TRACE_PLAY_UNHANDLED,
                   packet_id,
                   (int32_t)packet->body_len,
                   0,
                   0u);
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

static int partial_frame_has_invalid_packet_id(const mc_server_t *server)
{
    int32_t body_len = 0;
    int32_t packet_id = 0;
    size_t prefix_len = 0;
    size_t used = 0;
    size_t body_available = 0;

    if (!mc_varint_decode(server->rx_accum, server->rx_accum_len, &body_len, &prefix_len)) {
        return 0;
    }
    if (body_len < 0 || (size_t)body_len > MC_MAX_PACKET_BODY) {
        return 1;
    }
    if (server->rx_accum_len <= prefix_len) {
        return 0;
    }

    body_available = server->rx_accum_len - prefix_len;
#if MC_PROTOCOL_COMPRESSION_ENABLE
    if (server->compression_enabled) {
        int32_t data_len = 0;

        if (!mc_varint_decode(server->rx_accum + prefix_len, body_available, &data_len, &used)) {
            return body_available >= 5u;
        }
        if (data_len != 0) {
            return 1;
        }
        if (body_available <= used) {
            return 0;
        }
        body_available -= used;
        if (!mc_varint_decode(server->rx_accum + prefix_len + used, body_available, &packet_id, &used)) {
            return body_available >= 5u;
        }
        return packet_id < 0 || packet_id > MC_MAX_SERVERBOUND_PACKET_ID;
    }
#else
    (void)server;
#endif
    if (!mc_varint_decode(server->rx_accum + prefix_len, body_available, &packet_id, &used)) {
        return body_available >= 5u;
    }
    return packet_id < 0 || packet_id > MC_MAX_SERVERBOUND_PACKET_ID;
}

int mc_server_receive(mc_server_t *server, const uint8_t *bytes, size_t len, mc_ringbuf_t *tx)
{
    for (size_t i = 0; i < len; i++) {
        if (server->rx_accum_len == sizeof(server->rx_accum)) {
            server->rx_accum_len = 0;
            return 0;
        }
        server->rx_accum[server->rx_accum_len++] = bytes[i];
    }

    for (;;) {
        mc_packet_t packet;
        size_t frame_len = 0;
        mc_frame_status_t status = classify_frame(server->rx_accum, server->rx_accum_len, &frame_len);
        if (status == MC_FRAME_NEED_MORE) {
            if (partial_frame_has_invalid_packet_id(server)) {
                server->rx_accum_len = 0;
                return 0;
            }
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
        if (server->compression_enabled) {
            const uint8_t *body = 0;
            size_t body_len = 0;
            int32_t data_len = 0;
            mc_packet_t compressed_packet;

            if (!mc_packet_get_compressed_body(packet.body, packet.body_len, &body, &body_len, &data_len)) {
                server->rx_accum_len = 0;
                return 0;
            }
            if (data_len != 0) {
                server->rx_accum_len = 0;
                return 0;
            }
            compressed_packet.body = body;
            compressed_packet.body_len = body_len;
            compressed_packet.frame_len = body_len;
            if (!handle_packet(server, &compressed_packet, tx)) {
                server->rx_accum_len = 0;
                return 0;
            }
        } else if (!handle_packet(server, &packet, tx)) {
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

int mc_server_tick_at(mc_server_t *server, mc_ringbuf_t *tx, uint32_t now_ticks)
{
    server->ticks++;
    if (server->state == MC_CONN_PLAY && !server->play_bootstrap_sent) {
        while (server->play_bootstrap_stage < MC_BOOTSTRAP_DONE) {
            int queued = 0;
            int advance_stage = 1;
            switch (server->play_bootstrap_stage) {
            case MC_BOOTSTRAP_JOIN_GAME:
                queued = queue_join_game(server, tx);
                break;
            case MC_BOOTSTRAP_SPAWN_POSITION:
                queued = queue_spawn_position(server, tx);
                break;
            case MC_BOOTSTRAP_TIME:
                queued = queue_time(server, tx);
                break;
            case MC_BOOTSTRAP_HEALTH:
                queued = queue_health(server, tx);
                break;
            case MC_BOOTSTRAP_POSITION:
                queued = queue_position(server, tx);
                break;
            case MC_BOOTSTRAP_CHUNKS:
#if MC_PROTOCOL_COMPRESSION_ENABLE && MC_USE_PSRAM_COMPRESSED_MAP
                if (server->compression_enabled) {
                    queued = mc_world_queue_compressed_spawn_chunk(tx, server->play_bootstrap_chunk_index);
                } else {
                    queued = mc_world_queue_spawn_chunk(tx, server->play_bootstrap_chunk_index);
                }
#elif MC_PROTOCOL_COMPRESSION_ENABLE
                queued = queue_spawn_chunk_auto(server, tx, server->play_bootstrap_chunk_index);
#else
                queued = mc_world_queue_spawn_chunk(tx, server->play_bootstrap_chunk_index);
#endif
                if (queued) {
                    size_t chunk_count = mc_world_spawn_chunk_count();
#if MC_USE_PSRAM_COMPRESSED_MAP
                    if (server->compression_enabled) {
                        chunk_count = mc_world_compressed_chunk_count();
                    }
#endif
                    server->play_bootstrap_chunk_index++;
                    if ((size_t)server->play_bootstrap_chunk_index < chunk_count) {
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
    return tick_keepalive(server, tx, now_ticks);
}

int mc_server_tick(mc_server_t *server, mc_ringbuf_t *tx)
{
    return mc_server_tick_at(server, tx, server->ticks + 1u);
}
