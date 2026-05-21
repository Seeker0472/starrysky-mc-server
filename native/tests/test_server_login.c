#include <stdint.h>
#include "mc_commands.h"
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
#define ASSERT_STREQ(a, b) do { if (strcmp((a), (b)) != 0) return 1; } while (0)

static int double_close(double actual, double expected)
{
    double diff = actual > expected ? actual - expected : expected - actual;

    return diff < 0.000001;
}

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

static int read_clientbound_keepalive_id(const uint8_t *src, size_t src_len, int32_t *keepalive_id)
{
    mc_packet_t packet;
    const uint8_t *body = 0;
    size_t body_len = 0;
    size_t pos = 0;
    int32_t packet_id = -1;

    if (!mc_packet_try_read(src, src_len, &packet)) {
        return 0;
    }
#if MC_PROTOCOL_COMPRESSION_ENABLE
    {
        int32_t data_len = -1;
        if (!mc_packet_get_compressed_body(packet.body, packet.body_len, &body, &body_len, &data_len) ||
            data_len != 0) {
            return 0;
        }
    }
#else
    body = packet.body;
    body_len = packet.body_len;
#endif
    if (!mc_varint_decode(body + pos, body_len - pos, &packet_id, &pos) || packet_id != 0x00) {
        return 0;
    }
    return mc_varint_decode(body + pos, body_len - pos, keepalive_id, &pos);
}

static int read_clientbound_body(const uint8_t *src,
                                 size_t src_len,
                                 const uint8_t **body,
                                 size_t *body_len)
{
    mc_packet_t packet;

    if (!mc_packet_try_read(src, src_len, &packet)) {
        return 0;
    }
#if MC_PROTOCOL_COMPRESSION_ENABLE
    {
        int32_t data_len = -1;
        if (!mc_packet_get_compressed_body(packet.body, packet.body_len, body, body_len, &data_len) ||
            data_len != 0) {
            return 0;
        }
    }
#else
    *body = packet.body;
    *body_len = packet.body_len;
#endif
    return 1;
}

static int read_clientbound_abilities_flags(const uint8_t *src, size_t src_len, uint8_t *flags)
{
    const uint8_t *body = 0;
    size_t body_len = 0;
    size_t pos = 0;
    size_t used = 0;
    int32_t packet_id = -1;

    if (!read_clientbound_body(src, src_len, &body, &body_len) ||
        !mc_varint_decode(body + pos, body_len - pos, &packet_id, &used) ||
        packet_id != 0x39) {
        return 0;
    }
    pos += used;
    if (pos >= body_len) {
        return 0;
    }
    *flags = body[pos];
    return 1;
}

static int read_be64_at(const uint8_t *src, size_t src_len, size_t *pos, uint64_t *value)
{
    uint64_t v = 0u;

    if (*pos > src_len || src_len - *pos < 8u) {
        return 0;
    }
    for (size_t i = 0; i < 8u; i++) {
        v = (v << 8) | (uint64_t)src[*pos + i];
    }
    *value = v;
    *pos += 8u;
    return 1;
}

static int read_be32_at(const uint8_t *src, size_t src_len, size_t *pos, uint32_t *value)
{
    if (*pos > src_len || src_len - *pos < 4u) {
        return 0;
    }
    *value = ((uint32_t)src[*pos] << 24) |
             ((uint32_t)src[*pos + 1u] << 16) |
             ((uint32_t)src[*pos + 2u] << 8) |
             (uint32_t)src[*pos + 3u];
    *pos += 4u;
    return 1;
}

static int read_f32_at(const uint8_t *src, size_t src_len, size_t *pos, float *value)
{
    union { uint32_t u; float f; } u;

    if (!read_be32_at(src, src_len, pos, &u.u)) {
        return 0;
    }
    *value = u.f;
    return 1;
}

static int read_f64_at(const uint8_t *src, size_t src_len, size_t *pos, double *value)
{
    union { uint64_t u; double d; } u;

    if (!read_be64_at(src, src_len, pos, &u.u)) {
        return 0;
    }
    *value = u.d;
    return 1;
}

static int read_i64_at(const uint8_t *src, size_t src_len, size_t *pos, int64_t *value)
{
    uint64_t raw = 0u;

    if (!read_be64_at(src, src_len, pos, &raw)) {
        return 0;
    }
    *value = (int64_t)raw;
    return 1;
}

static int read_clientbound_position(const uint8_t *src,
                                     size_t src_len,
                                     double *x,
                                     double *y,
                                     double *z)
{
    const uint8_t *body = 0;
    size_t body_len = 0;
    size_t pos = 0;
    size_t used = 0;
    int32_t packet_id = -1;

    if (!read_clientbound_body(src, src_len, &body, &body_len) ||
        !mc_varint_decode(body + pos, body_len - pos, &packet_id, &used) ||
        packet_id != 0x08) {
        return 0;
    }
    pos += used;
    return read_f64_at(body, body_len, &pos, x) &&
           read_f64_at(body, body_len, &pos, y) &&
           read_f64_at(body, body_len, &pos, z);
}

static int read_clientbound_time(const uint8_t *src, size_t src_len, int32_t *time_of_day)
{
    const uint8_t *body = 0;
    size_t body_len = 0;
    size_t pos = 0;
    size_t used = 0;
    int32_t packet_id = -1;
    int64_t world_age = 0;
    int64_t day_time = 0;

    if (!read_clientbound_body(src, src_len, &body, &body_len) ||
        !mc_varint_decode(body + pos, body_len - pos, &packet_id, &used) ||
        packet_id != 0x03) {
        return 0;
    }
    pos += used;
    if (!read_i64_at(body, body_len, &pos, &world_age) ||
        !read_i64_at(body, body_len, &pos, &day_time) ||
        pos != body_len ||
        day_time < INT32_MIN ||
        day_time > INT32_MAX) {
        return 0;
    }
    (void)world_age;
    *time_of_day = (int32_t)day_time;
    return 1;
}

typedef struct {
    uint8_t reason;
    float value;
} game_state_packet_t;

static int read_clientbound_game_states(const uint8_t *src,
                                        size_t src_len,
                                        game_state_packet_t *states,
                                        size_t state_cap,
                                        size_t *state_count)
{
    size_t frame_pos = 0;

    *state_count = 0u;
    while (frame_pos < src_len) {
        const uint8_t *body = 0;
        size_t body_len = 0;
        size_t body_pos = 0;
        size_t used = 0;
        size_t frame_len = clientbound_frame_len(src + frame_pos, src_len - frame_pos);
        int32_t packet_id = -1;

        if (frame_len == 0u ||
            !read_clientbound_body(src + frame_pos, src_len - frame_pos, &body, &body_len) ||
            !mc_varint_decode(body + body_pos, body_len - body_pos, &packet_id, &used)) {
            return 0;
        }
        body_pos += used;
        if (packet_id == 0x2b) {
            if (*state_count >= state_cap || body_len - body_pos != 5u) {
                return 0;
            }
            states[*state_count].reason = body[body_pos];
            body_pos++;
            if (!read_f32_at(body, body_len, &body_pos, &states[*state_count].value) ||
                body_pos != body_len) {
                return 0;
            }
            (*state_count)++;
        }
        frame_pos += frame_len;
    }

    return 1;
}

static int assert_weather_game_states(const uint8_t *src,
                                      size_t src_len,
                                      const game_state_packet_t *expected,
                                      size_t expected_count)
{
    game_state_packet_t actual[4];
    size_t actual_count = 0u;

    ASSERT_TRUE(read_clientbound_game_states(src,
                                            src_len,
                                            actual,
                                            sizeof(actual) / sizeof(actual[0]),
                                            &actual_count));
    ASSERT_EQ(actual_count, expected_count);
    for (size_t i = 0u; i < expected_count; i++) {
        ASSERT_EQ(actual[i].reason, expected[i].reason);
        ASSERT_TRUE(actual[i].value == expected[i].value);
    }
    return 0;
}

static int read_string_at(const uint8_t *src,
                          size_t src_len,
                          size_t *pos,
                          char *dst,
                          size_t dst_cap)
{
    int32_t len = 0;
    size_t used = 0;

    if (!mc_varint_decode(src + *pos, src_len - *pos, &len, &used) || len < 0) {
        return 0;
    }
    *pos += used;
    if ((size_t)len >= dst_cap || *pos + (size_t)len > src_len) {
        return 0;
    }
    memcpy(dst, src + *pos, (size_t)len);
    dst[len] = '\0';
    *pos += (size_t)len;
    return 1;
}

static int hex_value(char ch, uint8_t *value)
{
    if (ch >= '0' && ch <= '9') {
        *value = (uint8_t)(ch - '0');
        return 1;
    }
    if (ch >= 'a' && ch <= 'f') {
        *value = (uint8_t)(ch - 'a' + 10);
        return 1;
    }
    if (ch >= 'A' && ch <= 'F') {
        *value = (uint8_t)(ch - 'A' + 10);
        return 1;
    }
    return 0;
}

static int read_clientbound_chat_text(const uint8_t *src,
                                      size_t src_len,
                                      char *dst,
                                      size_t dst_cap,
                                      uint8_t *position)
{
    const char prefix[] = "{\"text\":\"";
    const char suffix[] = "\"}";
    const uint8_t *body = 0;
    size_t body_len = 0;
    size_t pos = 0;
    size_t used = 0;
    size_t json_len;
    size_t text_len;
    int32_t packet_id = -1;
    char json[MC_COMMAND_TEXT_CAP * 6u + 16u];

    if (!read_clientbound_body(src, src_len, &body, &body_len) ||
        !mc_varint_decode(body + pos, body_len - pos, &packet_id, &used) ||
        packet_id != 0x02) {
        return 0;
    }
    pos += used;
    if (!read_string_at(body, body_len, &pos, json, sizeof(json)) || pos >= body_len) {
        return 0;
    }
    *position = body[pos];
    pos++;
    if (pos != body_len) {
        return 0;
    }

    json_len = strlen(json);
    if (json_len < sizeof(prefix) - 1u + sizeof(suffix) - 1u ||
        memcmp(json, prefix, sizeof(prefix) - 1u) != 0 ||
        memcmp(json + json_len - (sizeof(suffix) - 1u), suffix, sizeof(suffix) - 1u) != 0) {
        return 0;
    }

    text_len = json_len - (sizeof(prefix) - 1u) - (sizeof(suffix) - 1u);
    {
        const char *json_text = json + sizeof(prefix) - 1u;
        size_t dst_len = 0u;

        for (size_t i = 0u; i < text_len; i++) {
            char ch = json_text[i];
            if (ch == '\\') {
                i++;
                if (i >= text_len) {
                    return 0;
                }
                ch = json_text[i];
                if (ch == 'u') {
                    uint8_t hi;
                    uint8_t lo;

                    if (i + 4u >= text_len ||
                        json_text[i + 1u] != '0' ||
                        json_text[i + 2u] != '0' ||
                        !hex_value(json_text[i + 3u], &hi) ||
                        !hex_value(json_text[i + 4u], &lo)) {
                        return 0;
                    }
                    ch = (char)((hi << 4) | lo);
                    i += 4u;
                } else if (ch != '"' && ch != '\\') {
                    return 0;
                }
            } else if (ch == '"' || (uint8_t)ch < 0x20u) {
                return 0;
            }
            if (dst_len + 1u >= dst_cap) {
                return 0;
            }
            dst[dst_len] = ch;
            dst_len++;
        }
        dst[dst_len] = '\0';
    }
    return 1;
}

static int enter_play(mc_server_t *server, mc_ringbuf_t *tx)
{
    uint8_t out[MC_TX_RING_CAP];
    const uint8_t handshake_login[] = {
        0x0f, 0x00, 0x2f, 0x09, '1','2','7','.','0','.','0','.','1', 0x63, 0xdd, 0x02
    };
    const uint8_t login_start[] = {
        0x09, 0x00, 0x07, 'p','l','a','y','e','r','1'
    };

#if MC_PROTOCOL_COMPRESSION_ENABLE && MC_USE_PSRAM_COMPRESSED_MAP
    {
        static uint8_t compressed_arena[4096];
        if (!mc_world_compressed_init(compressed_arena, sizeof(compressed_arena))) {
            return 0;
        }
    }
#endif
    mc_server_init(server);
    if (!mc_server_receive(server, handshake_login, sizeof(handshake_login), tx)) {
        return 0;
    }
    if (!mc_server_receive(server, login_start, sizeof(login_start), tx)) {
        return 0;
    }
    (void)mc_ringbuf_read(tx, out, sizeof(out));
    return server->state == MC_CONN_PLAY;
}

static void write_be32(uint8_t *dst, uint32_t value)
{
    dst[0] = (uint8_t)(value >> 24);
    dst[1] = (uint8_t)(value >> 16);
    dst[2] = (uint8_t)(value >> 8);
    dst[3] = (uint8_t)value;
}

static void write_be64(uint8_t *dst, uint64_t value)
{
    dst[0] = (uint8_t)(value >> 56);
    dst[1] = (uint8_t)(value >> 48);
    dst[2] = (uint8_t)(value >> 40);
    dst[3] = (uint8_t)(value >> 32);
    dst[4] = (uint8_t)(value >> 24);
    dst[5] = (uint8_t)(value >> 16);
    dst[6] = (uint8_t)(value >> 8);
    dst[7] = (uint8_t)value;
}

static void write_f32_be(uint8_t *dst, float value)
{
    union { float f; uint32_t u; } u;
    u.f = value;
    write_be32(dst, u.u);
}

static void write_f64_be(uint8_t *dst, double value)
{
    union { double d; uint64_t u; } u;
    u.d = value;
    write_be64(dst, u.u);
}

static size_t wrap_serverbound_play_body(const uint8_t *body, size_t body_len, uint8_t *dst, size_t dst_cap)
{
#if MC_PROTOCOL_COMPRESSION_ENABLE
    return mc_packet_wrap_compressed_plain(body, body_len, dst, dst_cap);
#else
    return mc_packet_wrap(body, body_len, dst, dst_cap);
#endif
}

static size_t write_serverbound_chat_frame(const char *message, uint8_t *frame, size_t frame_cap)
{
    uint8_t body[MC_COMMAND_TEXT_CAP + 8u];
    mc_writer_t w;

    mc_writer_init(&w, body, sizeof(body));
    if (!mc_write_varint(&w, 0x01) ||
        !mc_write_string(&w, message)) {
        return 0u;
    }
    return wrap_serverbound_play_body(body, w.len, frame, frame_cap);
}

static size_t write_serverbound_chat_bytes_frame(const uint8_t *message,
                                                 size_t message_len,
                                                 uint8_t *frame,
                                                 size_t frame_cap)
{
    uint8_t body[MC_COMMAND_TEXT_CAP + 8u];
    mc_writer_t w;

    mc_writer_init(&w, body, sizeof(body));
    if (!mc_write_varint(&w, 0x01) ||
        !mc_write_varint(&w, (int32_t)message_len) ||
        !mc_write_bytes(&w, message, message_len)) {
        return 0u;
    }
    return wrap_serverbound_play_body(body, w.len, frame, frame_cap);
}

static size_t write_login_start_bytes_frame(const uint8_t *username,
                                            size_t username_len,
                                            uint8_t *frame,
                                            size_t frame_cap)
{
    uint8_t body[MC_MAX_USERNAME + 8u];
    mc_writer_t w;

    mc_writer_init(&w, body, sizeof(body));
    if (!mc_write_varint(&w, 0x00) ||
        !mc_write_varint(&w, (int32_t)username_len) ||
        !mc_write_bytes(&w, username, username_len)) {
        return 0u;
    }
    return mc_packet_wrap(body, w.len, frame, frame_cap);
}

typedef struct {
    double x;
    double y;
    double z;
    float yaw;
    float pitch;
    uint8_t on_ground;
} player_state_snapshot_t;

static player_state_snapshot_t snapshot_player_state(const mc_server_t *server)
{
    player_state_snapshot_t snapshot;
    snapshot.x = server->player_x;
    snapshot.y = server->player_y;
    snapshot.z = server->player_z;
    snapshot.yaw = server->player_yaw;
    snapshot.pitch = server->player_pitch;
    snapshot.on_ground = server->player_on_ground;
    return snapshot;
}

static int assert_player_state_unchanged(const mc_server_t *server,
                                         const player_state_snapshot_t *snapshot)
{
    ASSERT_TRUE(server->player_x == snapshot->x);
    ASSERT_TRUE(server->player_y == snapshot->y);
    ASSERT_TRUE(server->player_z == snapshot->z);
    ASSERT_TRUE(server->player_yaw == snapshot->yaw);
    ASSERT_TRUE(server->player_pitch == snapshot->pitch);
    ASSERT_EQ(server->player_on_ground, snapshot->on_ground);
    return 0;
}

typedef struct {
    size_t action_bytes;
    size_t chat_bytes;
    size_t total_bytes;
} command_response_sizes_t;

static int finish_play_bootstrap(mc_server_t *server, mc_ringbuf_t *tx, uint32_t now_ticks);

static int measure_command_response_sizes(const char *command,
                                          int32_t action_packet_id,
                                          int expected_action_packets,
                                          command_response_sizes_t *sizes)
{
    mc_server_t server;
    uint8_t tx_storage[MC_TX_RING_CAP];
    mc_ringbuf_t tx;
    uint8_t frame[MC_COMMAND_TEXT_CAP + 16u];
    uint8_t out[MC_TX_RING_CAP];
    size_t frame_len;
    size_t out_len;
    size_t pos = 0u;
    int action_packets = 0;
    int chat_packets = 0;
    int32_t last_packet_id = -1;

    memset(sizes, 0, sizeof(*sizes));
    mc_ringbuf_init(&tx, tx_storage, sizeof(tx_storage));
    ASSERT_TRUE(enter_play(&server, &tx));
    ASSERT_TRUE(finish_play_bootstrap(&server, &tx, 1u));
    (void)drain_ring(&tx, out, sizeof(out));

    frame_len = write_serverbound_chat_frame(command, frame, sizeof(frame));
    ASSERT_TRUE(frame_len > 0u);
    ASSERT_TRUE(mc_server_receive(&server, frame, frame_len, &tx));

    out_len = drain_ring(&tx, out, sizeof(out));
    ASSERT_TRUE(out_len > 0u);
    sizes->total_bytes = out_len;

    while (pos < out_len) {
        size_t packet_len = clientbound_frame_len(out + pos, out_len - pos);
        int32_t packet_id = -1;

        ASSERT_TRUE(packet_len > 0u);
        ASSERT_TRUE(read_clientbound_packet_id(out + pos, out_len - pos, &packet_id));
        if (packet_id == action_packet_id) {
            sizes->action_bytes += packet_len;
            action_packets++;
        } else if (packet_id == 0x02) {
            sizes->chat_bytes += packet_len;
            chat_packets++;
        } else {
            return 1;
        }
        last_packet_id = packet_id;
        pos += packet_len;
    }

    ASSERT_EQ(action_packets, expected_action_packets);
    ASSERT_EQ(chat_packets, 1);
    ASSERT_EQ(last_packet_id, 0x02);
    ASSERT_TRUE(sizes->action_bytes > 0u);
    ASSERT_TRUE(sizes->chat_bytes > 0u);
    ASSERT_EQ(sizes->total_bytes, sizes->action_bytes + sizes->chat_bytes);
    return 0;
}

static int compute_command_failure_tx_cap(const char *command,
                                          int32_t action_packet_id,
                                          int expected_action_packets,
                                          size_t *tx_cap)
{
    command_response_sizes_t sizes;

    ASSERT_TRUE(!measure_command_response_sizes(command,
                                                action_packet_id,
                                                expected_action_packets,
                                                &sizes));
    *tx_cap = sizes.action_bytes;
    ASSERT_TRUE(*tx_cap >= sizes.action_bytes);
    ASSERT_TRUE(*tx_cap < sizes.total_bytes);
    return 0;
}

static int finish_play_bootstrap(mc_server_t *server, mc_ringbuf_t *tx, uint32_t now_ticks)
{
    uint8_t out[MC_TX_RING_CAP];
    size_t max_ticks = mc_world_spawn_chunk_count() + 8u;

    for (size_t i = 0; i < max_ticks && !server->play_bootstrap_sent; i++) {
        if (!mc_server_tick_at(server, tx, now_ticks)) {
            return 0;
        }
        (void)mc_ringbuf_read(tx, out, sizeof(out));
    }

    return server->play_bootstrap_sent != 0;
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

static int test_play_keepalive_queues_and_accepts_response(void)
{
    mc_server_t server;
    uint8_t tx_storage[MC_TX_RING_CAP];
    mc_ringbuf_t tx;
    uint8_t response_body[8];
    uint8_t response_frame[16];
    size_t response_body_len;
    size_t response_frame_len;
    int32_t keepalive_id = -1;
    mc_writer_t w;

    mc_ringbuf_init(&tx, tx_storage, sizeof(tx_storage));
    ASSERT_TRUE(enter_play(&server, &tx));
    ASSERT_TRUE(finish_play_bootstrap(&server, &tx, 1u));
    keepalive_id = server.keepalive_id;
    ASSERT_EQ(keepalive_id, 1);
    ASSERT_EQ(server.keepalive_pending, 1u);

    mc_writer_init(&w, response_body, sizeof(response_body));
    ASSERT_TRUE(mc_write_varint(&w, 0x00));
    ASSERT_TRUE(mc_write_varint(&w, keepalive_id));
    response_body_len = w.len;
#if MC_PROTOCOL_COMPRESSION_ENABLE
    response_frame_len = mc_packet_wrap_compressed_plain(response_body,
                                                        response_body_len,
                                                        response_frame,
                                                        sizeof(response_frame));
#else
    response_frame_len = mc_packet_wrap(response_body,
                                        response_body_len,
                                        response_frame,
                                        sizeof(response_frame));
#endif
    ASSERT_TRUE(response_frame_len > 0u);
    ASSERT_TRUE(mc_server_receive(&server, response_frame, response_frame_len, &tx));
    ASSERT_EQ(server.keepalive_pending, 0u);
    return 0;
}

static int test_play_keepalive_unanswered_requests_keep_incrementing(void)
{
    mc_server_t server;
    uint8_t tx_storage[MC_TX_RING_CAP];
    mc_ringbuf_t tx;
    uint8_t out[128];
    size_t out_len;
    int32_t keepalive_id = -1;
    uint32_t now = 1u;

    mc_ringbuf_init(&tx, tx_storage, sizeof(tx_storage));
    ASSERT_TRUE(enter_play(&server, &tx));
    ASSERT_TRUE(finish_play_bootstrap(&server, &tx, now));
    ASSERT_EQ(server.keepalive_id, 1);
    ASSERT_TRUE(mc_server_take_tx_reset(&server));
    ASSERT_TRUE(!mc_server_take_tx_reset(&server));

    for (uint8_t i = 0; i < 4u; i++) {
        now += MC_KEEPALIVE_INTERVAL_TICKS;
        ASSERT_TRUE(mc_server_tick_at(&server, &tx, now));
        out_len = drain_ring(&tx, out, sizeof(out));
        ASSERT_TRUE(out_len > 0u);
        ASSERT_TRUE(read_clientbound_keepalive_id(out, out_len, &keepalive_id));
        ASSERT_EQ(keepalive_id, (int32_t)i + 2);
        ASSERT_TRUE(!mc_server_take_tx_reset(&server));
        ASSERT_EQ(server.state, MC_CONN_PLAY);
        ASSERT_EQ(server.keepalive_pending, 1u);
    }

    ASSERT_EQ(server.state, MC_CONN_PLAY);
    ASSERT_EQ(server.keepalive_pending, 1u);
    ASSERT_EQ(mc_ringbuf_len(&tx), 0u);
    return 0;
}

static int test_play_keepalive_activity_is_not_required_for_liveness(void)
{
    mc_server_t server;
    uint8_t tx_storage[MC_TX_RING_CAP];
    mc_ringbuf_t tx;
    uint8_t out[128];
    uint8_t body[8];
    uint8_t frame[16];
    size_t frame_len;
    uint32_t now = 1u;
    int32_t keepalive_id = -1;
    mc_writer_t w;

    mc_ringbuf_init(&tx, tx_storage, sizeof(tx_storage));
    ASSERT_TRUE(enter_play(&server, &tx));
    ASSERT_TRUE(finish_play_bootstrap(&server, &tx, now));
    ASSERT_EQ(server.keepalive_id, 1);
    ASSERT_TRUE(mc_server_take_tx_reset(&server));
    ASSERT_TRUE(!mc_server_take_tx_reset(&server));

    now += MC_KEEPALIVE_INTERVAL_TICKS;
    ASSERT_TRUE(mc_server_tick_at(&server, &tx, now));
    ASSERT_EQ(server.keepalive_pending, 1u);
    ASSERT_TRUE(read_clientbound_keepalive_id(out,
                                             drain_ring(&tx, out, sizeof(out)),
                                             &keepalive_id));
    ASSERT_EQ(keepalive_id, 2);

    for (uint8_t i = 0; i < 4u; i++) {
        mc_writer_init(&w, body, sizeof(body));
        ASSERT_TRUE(mc_write_varint(&w, 0x03));
        ASSERT_TRUE(mc_write_bool(&w, (uint8_t)(i & 1u)));
        frame_len = wrap_serverbound_play_body(body, w.len, frame, sizeof(frame));
        ASSERT_TRUE(frame_len > 0u);
        ASSERT_TRUE(mc_server_receive(&server, frame, frame_len, &tx));
        ASSERT_EQ(server.keepalive_pending, 1u);

        now += MC_KEEPALIVE_INTERVAL_TICKS;
        ASSERT_TRUE(mc_server_tick_at(&server, &tx, now));
        ASSERT_TRUE(server.keepalive_pending);
        ASSERT_TRUE(!mc_server_take_tx_reset(&server));
        ASSERT_TRUE(read_clientbound_keepalive_id(out,
                                                 drain_ring(&tx, out, sizeof(out)),
                                                 &keepalive_id));
        ASSERT_EQ(keepalive_id, (int32_t)i + 3);
    }

    ASSERT_EQ(server.state, MC_CONN_PLAY);
    return 0;
}

static int test_play_keepalive_waits_for_bootstrap_complete(void)
{
    mc_server_t server;
    uint8_t tx_storage[MC_TX_RING_CAP];
    mc_ringbuf_t tx;
    uint8_t out[MC_TX_RING_CAP];
    size_t out_len;

    mc_ringbuf_init(&tx, tx_storage, sizeof(tx_storage));
    ASSERT_TRUE(enter_play(&server, &tx));

    ASSERT_TRUE(mc_server_tick_at(&server, &tx, 1u));
    ASSERT_TRUE(!server.play_bootstrap_sent);
    (void)drain_ring(&tx, out, sizeof(out));

    ASSERT_TRUE(mc_server_tick_at(&server, &tx, MC_KEEPALIVE_INTERVAL_TICKS + 1u));
    ASSERT_TRUE(!server.play_bootstrap_sent);
    out_len = drain_ring(&tx, out, sizeof(out));
    ASSERT_TRUE(out_len > 0u);
    ASSERT_EQ(count_packet_id(out, out_len, 0x00), 0);
    ASSERT_EQ(server.keepalive_pending, 0u);
    return 0;
}

static int test_play_keepalive_rolls_over_to_positive_id(void)
{
    mc_server_t server;
    uint8_t tx_storage[MC_TX_RING_CAP];
    mc_ringbuf_t tx;
    uint8_t out[128];
    size_t out_len;
    int32_t keepalive_id = -1;

    mc_ringbuf_init(&tx, tx_storage, sizeof(tx_storage));
    ASSERT_TRUE(enter_play(&server, &tx));
    ASSERT_TRUE(finish_play_bootstrap(&server, &tx, 1u));
    server.keepalive_id = INT32_MAX;

    ASSERT_TRUE(mc_server_tick_at(&server, &tx, MC_KEEPALIVE_INTERVAL_TICKS + 1u));
    out_len = drain_ring(&tx, out, sizeof(out));
    ASSERT_TRUE(out_len > 0u);
    ASSERT_TRUE(read_clientbound_keepalive_id(out, out_len, &keepalive_id));
    ASSERT_EQ(keepalive_id, 1);
    return 0;
}

static int test_play_keepalive_enqueue_failure_preserves_next_id(void)
{
    mc_server_t server;
    uint8_t tx_storage[MC_TX_RING_CAP];
    uint8_t small_tx_storage[1];
    mc_ringbuf_t tx;
    mc_ringbuf_t small_tx;
    uint8_t response_body[8];
    uint8_t response_frame[16];
    const uint8_t filler = 0xaa;
    size_t response_body_len;
    size_t response_frame_len;
    uint32_t now = 1u;
    int32_t keepalive_id = -1;
    mc_writer_t w;

    mc_ringbuf_init(&tx, tx_storage, sizeof(tx_storage));
    ASSERT_TRUE(enter_play(&server, &tx));
    ASSERT_TRUE(finish_play_bootstrap(&server, &tx, now));

    keepalive_id = server.keepalive_id;
    ASSERT_EQ(keepalive_id, 1);
    ASSERT_EQ(server.keepalive_id, keepalive_id);
    ASSERT_EQ(server.keepalive_pending, 1u);

    mc_ringbuf_init(&small_tx, small_tx_storage, sizeof(small_tx_storage));
    ASSERT_EQ(mc_ringbuf_write(&small_tx, &filler, 1u), 1u);
    now += MC_KEEPALIVE_INTERVAL_TICKS;
    ASSERT_TRUE(!mc_server_tick_at(&server, &small_tx, now));
    ASSERT_EQ(server.keepalive_id, keepalive_id);
    ASSERT_EQ(server.keepalive_pending, 1u);

    mc_writer_init(&w, response_body, sizeof(response_body));
    ASSERT_TRUE(mc_write_varint(&w, 0x00));
    ASSERT_TRUE(mc_write_varint(&w, keepalive_id));
    response_body_len = w.len;
#if MC_PROTOCOL_COMPRESSION_ENABLE
    response_frame_len = mc_packet_wrap_compressed_plain(response_body,
                                                        response_body_len,
                                                        response_frame,
                                                        sizeof(response_frame));
#else
    response_frame_len = mc_packet_wrap(response_body,
                                        response_body_len,
                                        response_frame,
                                        sizeof(response_frame));
#endif
    ASSERT_TRUE(response_frame_len > 0u);
    ASSERT_TRUE(mc_server_receive(&server, response_frame, response_frame_len, &tx));
    ASSERT_EQ(server.keepalive_pending, 0u);
    return 0;
}

static int test_play_keepalive_uses_new_id_each_interval(void)
{
    mc_server_t server;
    uint8_t tx_storage[MC_TX_RING_CAP];
    mc_ringbuf_t tx;
    uint8_t out[128];
    uint8_t response_body[8];
    uint8_t response_frame[16];
    size_t out_len;
    size_t response_body_len;
    size_t response_frame_len;
    uint32_t now = 1u;
    int32_t keepalive_id = -1;
    int32_t next_id = -1;
    mc_writer_t w;

    mc_ringbuf_init(&tx, tx_storage, sizeof(tx_storage));
    ASSERT_TRUE(enter_play(&server, &tx));
    ASSERT_TRUE(finish_play_bootstrap(&server, &tx, now));

    keepalive_id = server.keepalive_id;
    ASSERT_EQ(keepalive_id, 1);
    ASSERT_EQ(server.keepalive_pending, 1u);

    now += MC_KEEPALIVE_INTERVAL_TICKS;
    ASSERT_TRUE(mc_server_tick_at(&server, &tx, now));
    out_len = drain_ring(&tx, out, sizeof(out));
    ASSERT_TRUE(out_len > 0u);
    ASSERT_TRUE(read_clientbound_keepalive_id(out, out_len, &next_id));
    ASSERT_EQ(next_id, keepalive_id + 1);
    ASSERT_EQ(server.keepalive_pending, 1u);

    mc_writer_init(&w, response_body, sizeof(response_body));
    ASSERT_TRUE(mc_write_varint(&w, 0x00));
    ASSERT_TRUE(mc_write_varint(&w, keepalive_id));
    response_body_len = w.len;
#if MC_PROTOCOL_COMPRESSION_ENABLE
    response_frame_len = mc_packet_wrap_compressed_plain(response_body,
                                                        response_body_len,
                                                        response_frame,
                                                        sizeof(response_frame));
#else
    response_frame_len = mc_packet_wrap(response_body,
                                        response_body_len,
                                        response_frame,
                                        sizeof(response_frame));
#endif
    ASSERT_TRUE(response_frame_len > 0u);
    ASSERT_TRUE(mc_server_receive(&server, response_frame, response_frame_len, &tx));
    ASSERT_EQ(server.keepalive_pending, 0u);
    return 0;
}

static int test_play_chat_echoes_to_self(void)
{
    mc_server_t server;
    uint8_t tx_storage[MC_TX_RING_CAP];
    mc_ringbuf_t tx;
    uint8_t frame[MC_COMMAND_TEXT_CAP + 16u];
    uint8_t out[256];
    char text[MC_COMMAND_TEXT_CAP];
    uint8_t position = 0xffu;
    size_t frame_len;
    size_t out_len;

    mc_ringbuf_init(&tx, tx_storage, sizeof(tx_storage));
    ASSERT_TRUE(enter_play(&server, &tx));
    ASSERT_TRUE(finish_play_bootstrap(&server, &tx, 1u));
    (void)drain_ring(&tx, out, sizeof(out));

    frame_len = write_serverbound_chat_frame("hello", frame, sizeof(frame));
    ASSERT_TRUE(frame_len > 0u);
    ASSERT_TRUE(mc_server_receive(&server, frame, frame_len, &tx));

    out_len = drain_ring(&tx, out, sizeof(out));
    ASSERT_TRUE(out_len > 0u);
    ASSERT_EQ(clientbound_frame_len(out, out_len), out_len);
    ASSERT_EQ(count_packet_id(out, out_len, 0x02), 1);
    ASSERT_TRUE(read_clientbound_chat_text(out, out_len, text, sizeof(text), &position));
    ASSERT_EQ(position, 0u);
    ASSERT_STREQ(text, "<player1> hello");
    return 0;
}

static int test_play_chat_escapes_quote_and_backslash(void)
{
    mc_server_t server;
    uint8_t tx_storage[MC_TX_RING_CAP];
    mc_ringbuf_t tx;
    uint8_t frame[MC_COMMAND_TEXT_CAP + 16u];
    uint8_t out[256];
    char text[MC_COMMAND_TEXT_CAP];
    uint8_t position = 0xffu;
    size_t frame_len;
    size_t out_len;

    mc_ringbuf_init(&tx, tx_storage, sizeof(tx_storage));
    ASSERT_TRUE(enter_play(&server, &tx));
    ASSERT_TRUE(finish_play_bootstrap(&server, &tx, 1u));
    (void)drain_ring(&tx, out, sizeof(out));

    frame_len = write_serverbound_chat_frame("say \"hi\" \\\\ path", frame, sizeof(frame));
    ASSERT_TRUE(frame_len > 0u);
    ASSERT_TRUE(mc_server_receive(&server, frame, frame_len, &tx));

    out_len = drain_ring(&tx, out, sizeof(out));
    ASSERT_TRUE(out_len > 0u);
    ASSERT_EQ(clientbound_frame_len(out, out_len), out_len);
    ASSERT_EQ(count_packet_id(out, out_len, 0x02), 1);
    ASSERT_TRUE(read_clientbound_chat_text(out, out_len, text, sizeof(text), &position));
    ASSERT_EQ(position, 0u);
    ASSERT_STREQ(text, "<player1> say \"hi\" \\\\ path");
    return 0;
}

static int test_play_chat_command_help_queues_chat_response(void)
{
    mc_server_t server;
    uint8_t tx_storage[MC_TX_RING_CAP];
    mc_ringbuf_t tx;
    uint8_t frame[MC_COMMAND_TEXT_CAP + 16u];
    uint8_t out[512];
    char text[MC_COMMAND_TEXT_CAP];
    uint8_t position = 0xffu;
    size_t frame_len;
    size_t out_len;

    mc_ringbuf_init(&tx, tx_storage, sizeof(tx_storage));
    ASSERT_TRUE(enter_play(&server, &tx));
    ASSERT_TRUE(finish_play_bootstrap(&server, &tx, 1u));
    (void)drain_ring(&tx, out, sizeof(out));

    frame_len = write_serverbound_chat_frame("/help", frame, sizeof(frame));
    ASSERT_TRUE(frame_len > 0u);
    ASSERT_TRUE(mc_server_receive(&server, frame, frame_len, &tx));

    out_len = drain_ring(&tx, out, sizeof(out));
    ASSERT_TRUE(out_len > 0u);
    ASSERT_EQ(clientbound_frame_len(out, out_len), out_len);
    ASSERT_EQ(count_packet_id(out, out_len, 0x02), 1);
    ASSERT_TRUE(read_clientbound_chat_text(out, out_len, text, sizeof(text), &position));
    ASSERT_EQ(position, 0u);
    ASSERT_STREQ(text,
                 "Commands: /help, /spawn, /tp <x> <y> <z>, /pos, /time <day|noon|night|midnight|ticks>, /weather <clear|rain|thunder>");
    return 0;
}

static int test_play_spawn_and_tp_commands_queue_position(void)
{
    mc_server_t server;
    uint8_t tx_storage[MC_TX_RING_CAP];
    mc_ringbuf_t tx;
    uint8_t frame[MC_COMMAND_TEXT_CAP + 16u];
    uint8_t out[512];
    size_t frame_len;
    size_t out_len;
    double x = 0.0;
    double y = 0.0;
    double z = 0.0;

    mc_ringbuf_init(&tx, tx_storage, sizeof(tx_storage));
    ASSERT_TRUE(enter_play(&server, &tx));
    ASSERT_TRUE(finish_play_bootstrap(&server, &tx, 1u));
    (void)drain_ring(&tx, out, sizeof(out));

    frame_len = write_serverbound_chat_frame("/tp 2.5 44 -3.5", frame, sizeof(frame));
    ASSERT_TRUE(frame_len > 0u);
    ASSERT_TRUE(mc_server_receive(&server, frame, frame_len, &tx));

    out_len = drain_ring(&tx, out, sizeof(out));
    ASSERT_TRUE(out_len > 0u);
    ASSERT_EQ(count_packet_id(out, out_len, 0x08), 1);
    ASSERT_TRUE(read_clientbound_position(out, out_len, &x, &y, &z));
    ASSERT_TRUE(double_close(x, 2.5));
    ASSERT_TRUE(double_close(y, 44.0));
    ASSERT_TRUE(double_close(z, -3.5));
    ASSERT_TRUE(double_close(server.player_x, 2.5));
    ASSERT_TRUE(double_close(server.player_y, 44.0));
    ASSERT_TRUE(double_close(server.player_z, -3.5));

    frame_len = write_serverbound_chat_frame("/spawn", frame, sizeof(frame));
    ASSERT_TRUE(frame_len > 0u);
    ASSERT_TRUE(mc_server_receive(&server, frame, frame_len, &tx));

    out_len = drain_ring(&tx, out, sizeof(out));
    ASSERT_TRUE(out_len > 0u);
    ASSERT_EQ(count_packet_id(out, out_len, 0x08), 1);
    ASSERT_TRUE(read_clientbound_position(out, out_len, &x, &y, &z));
    ASSERT_TRUE(x == MC_COMMAND_DEFAULT_X);
    ASSERT_TRUE(y == MC_COMMAND_DEFAULT_Y);
    ASSERT_TRUE(z == MC_COMMAND_DEFAULT_Z);
    ASSERT_TRUE(server.player_x == MC_COMMAND_DEFAULT_X);
    ASSERT_TRUE(server.player_y == MC_COMMAND_DEFAULT_Y);
    ASSERT_TRUE(server.player_z == MC_COMMAND_DEFAULT_Z);
    return 0;
}

static int test_play_time_command_queues_time_update(void)
{
    mc_server_t server;
    uint8_t tx_storage[MC_TX_RING_CAP];
    mc_ringbuf_t tx;
    uint8_t frame[MC_COMMAND_TEXT_CAP + 16u];
    uint8_t out[512];
    size_t frame_len;
    size_t out_len;
    int32_t time_of_day = -1;

    mc_ringbuf_init(&tx, tx_storage, sizeof(tx_storage));
    ASSERT_TRUE(enter_play(&server, &tx));
    ASSERT_TRUE(finish_play_bootstrap(&server, &tx, 1u));
    (void)drain_ring(&tx, out, sizeof(out));

    frame_len = write_serverbound_chat_frame("/time night", frame, sizeof(frame));
    ASSERT_TRUE(frame_len > 0u);
    ASSERT_TRUE(mc_server_receive(&server, frame, frame_len, &tx));

    out_len = drain_ring(&tx, out, sizeof(out));
    ASSERT_TRUE(out_len > 0u);
    ASSERT_EQ(count_packet_id(out, out_len, 0x03), 1);
    ASSERT_TRUE(read_clientbound_time(out, out_len, &time_of_day));
    ASSERT_EQ(time_of_day, 13000);
    ASSERT_EQ(server.world_time, 13000);
    return 0;
}

static int test_play_weather_command_queues_game_state(void)
{
    static const game_state_packet_t expected_rain[] = {
        {2u, 0.0f},
        {7u, 1.0f},
        {8u, 0.0f},
    };
    mc_server_t server;
    uint8_t tx_storage[MC_TX_RING_CAP];
    mc_ringbuf_t tx;
    uint8_t frame[MC_COMMAND_TEXT_CAP + 16u];
    uint8_t out[512];
    size_t frame_len;
    size_t out_len;

    mc_ringbuf_init(&tx, tx_storage, sizeof(tx_storage));
    ASSERT_TRUE(enter_play(&server, &tx));
    ASSERT_TRUE(finish_play_bootstrap(&server, &tx, 1u));
    (void)drain_ring(&tx, out, sizeof(out));

    frame_len = write_serverbound_chat_frame("/weather rain", frame, sizeof(frame));
    ASSERT_TRUE(frame_len > 0u);
    ASSERT_TRUE(mc_server_receive(&server, frame, frame_len, &tx));

    out_len = drain_ring(&tx, out, sizeof(out));
    ASSERT_TRUE(out_len > 0u);
    ASSERT_EQ(count_packet_id(out, out_len, 0x2b), 3);
    ASSERT_TRUE(!assert_weather_game_states(out,
                                            out_len,
                                            expected_rain,
                                            sizeof(expected_rain) / sizeof(expected_rain[0])));
    ASSERT_EQ(server.weather, MC_WEATHER_RAIN);
    return 0;
}

static int test_play_weather_clear_and_thunder_queue_expected_game_states(void)
{
    static const game_state_packet_t expected_clear[] = {
        {1u, 0.0f},
        {7u, 0.0f},
        {8u, 0.0f},
    };
    static const game_state_packet_t expected_thunder[] = {
        {2u, 0.0f},
        {7u, 1.0f},
        {8u, 1.0f},
    };
    mc_server_t server;
    uint8_t tx_storage[MC_TX_RING_CAP];
    mc_ringbuf_t tx;
    uint8_t frame[MC_COMMAND_TEXT_CAP + 16u];
    uint8_t out[512];
    size_t frame_len;
    size_t out_len;

    mc_ringbuf_init(&tx, tx_storage, sizeof(tx_storage));
    ASSERT_TRUE(enter_play(&server, &tx));
    ASSERT_TRUE(finish_play_bootstrap(&server, &tx, 1u));
    (void)drain_ring(&tx, out, sizeof(out));

    frame_len = write_serverbound_chat_frame("/weather clear", frame, sizeof(frame));
    ASSERT_TRUE(frame_len > 0u);
    ASSERT_TRUE(mc_server_receive(&server, frame, frame_len, &tx));
    out_len = drain_ring(&tx, out, sizeof(out));
    ASSERT_EQ(count_packet_id(out, out_len, 0x2b), 3);
    ASSERT_TRUE(!assert_weather_game_states(out,
                                            out_len,
                                            expected_clear,
                                            sizeof(expected_clear) / sizeof(expected_clear[0])));
    ASSERT_EQ(server.weather, MC_WEATHER_CLEAR);

    frame_len = write_serverbound_chat_frame("/weather thunder", frame, sizeof(frame));
    ASSERT_TRUE(frame_len > 0u);
    ASSERT_TRUE(mc_server_receive(&server, frame, frame_len, &tx));
    out_len = drain_ring(&tx, out, sizeof(out));
    ASSERT_EQ(count_packet_id(out, out_len, 0x2b), 3);
    ASSERT_TRUE(!assert_weather_game_states(out,
                                            out_len,
                                            expected_thunder,
                                            sizeof(expected_thunder) / sizeof(expected_thunder[0])));
    ASSERT_EQ(server.weather, MC_WEATHER_THUNDER);
    return 0;
}

static int test_play_command_queue_failure_preserves_state_and_tx(void)
{
    mc_server_t server;
    uint8_t tx_storage[MC_TX_RING_CAP];
    uint8_t small_tx_storage[MC_TX_RING_CAP];
    mc_ringbuf_t tx;
    mc_ringbuf_t small_tx;
    uint8_t frame[MC_COMMAND_TEXT_CAP + 16u];
    uint8_t out[512];
    size_t frame_len;
    size_t tp_tx_cap;
    size_t time_tx_cap;
    size_t weather_tx_cap;
    player_state_snapshot_t snapshot;

    ASSERT_TRUE(!compute_command_failure_tx_cap("/tp 10 20 30", 0x08, 1, &tp_tx_cap));
    ASSERT_TRUE(!compute_command_failure_tx_cap("/time night", 0x03, 1, &time_tx_cap));
    ASSERT_TRUE(!compute_command_failure_tx_cap("/weather rain", 0x2b, 3, &weather_tx_cap));

    mc_ringbuf_init(&tx, tx_storage, sizeof(tx_storage));
    ASSERT_TRUE(enter_play(&server, &tx));
    ASSERT_TRUE(finish_play_bootstrap(&server, &tx, 1u));
    (void)drain_ring(&tx, out, sizeof(out));

    snapshot = snapshot_player_state(&server);
    ASSERT_TRUE(snapshot.x == MC_COMMAND_DEFAULT_X);
    ASSERT_TRUE(snapshot.y == MC_COMMAND_DEFAULT_Y);
    ASSERT_TRUE(snapshot.z == MC_COMMAND_DEFAULT_Z);
    ASSERT_TRUE(snapshot.yaw == MC_COMMAND_DEFAULT_YAW);
    ASSERT_TRUE(snapshot.pitch == MC_COMMAND_DEFAULT_PITCH);
    frame_len = write_serverbound_chat_frame("/tp 10 20 30", frame, sizeof(frame));
    ASSERT_TRUE(frame_len > 0u);
    mc_ringbuf_init(&small_tx, small_tx_storage, tp_tx_cap);
    ASSERT_TRUE(!mc_server_receive(&server, frame, frame_len, &small_tx));
    ASSERT_EQ(mc_ringbuf_len(&small_tx), 0u);
    ASSERT_TRUE(!assert_player_state_unchanged(&server, &snapshot));

    frame_len = write_serverbound_chat_frame("/time night", frame, sizeof(frame));
    ASSERT_TRUE(frame_len > 0u);
    mc_ringbuf_init(&small_tx, small_tx_storage, time_tx_cap);
    ASSERT_TRUE(!mc_server_receive(&server, frame, frame_len, &small_tx));
    ASSERT_EQ(mc_ringbuf_len(&small_tx), 0u);
    ASSERT_EQ(server.world_time, MC_COMMAND_DEFAULT_TIME);

    frame_len = write_serverbound_chat_frame("/weather rain", frame, sizeof(frame));
    ASSERT_TRUE(frame_len > 0u);
    mc_ringbuf_init(&small_tx, small_tx_storage, weather_tx_cap);
    ASSERT_TRUE(!mc_server_receive(&server, frame, frame_len, &small_tx));
    ASSERT_EQ(mc_ringbuf_len(&small_tx), 0u);
    ASSERT_EQ(server.weather, MC_WEATHER_CLEAR);
    return 0;
}

static int test_play_chat_rejects_control_bytes(void)
{
    mc_server_t server;
    uint8_t tx_storage[MC_TX_RING_CAP];
    mc_ringbuf_t tx;
    uint8_t frame[MC_COMMAND_TEXT_CAP + 16u];
    uint8_t out[256];
    const uint8_t nul_chat[] = { 'b', 'a', 'd', 0x00, 'c', 'h', 'a', 't' };
    size_t frame_len;

    mc_ringbuf_init(&tx, tx_storage, sizeof(tx_storage));
    ASSERT_TRUE(enter_play(&server, &tx));
    ASSERT_TRUE(finish_play_bootstrap(&server, &tx, 1u));
    (void)drain_ring(&tx, out, sizeof(out));

    frame_len = write_serverbound_chat_frame("bad\nchat", frame, sizeof(frame));
    ASSERT_TRUE(frame_len > 0u);
    ASSERT_TRUE(!mc_server_receive(&server, frame, frame_len, &tx));
    ASSERT_EQ(drain_ring(&tx, out, sizeof(out)), 0u);

    ASSERT_TRUE(enter_play(&server, &tx));
    ASSERT_TRUE(finish_play_bootstrap(&server, &tx, 1u));
    (void)drain_ring(&tx, out, sizeof(out));

    frame_len = write_serverbound_chat_bytes_frame(nul_chat, sizeof(nul_chat), frame, sizeof(frame));
    ASSERT_TRUE(frame_len > 0u);
    ASSERT_TRUE(!mc_server_receive(&server, frame, frame_len, &tx));
    ASSERT_EQ(drain_ring(&tx, out, sizeof(out)), 0u);
    return 0;
}

static int test_play_chat_escapes_username_control_bytes(void)
{
    mc_server_t server;
    uint8_t tx_storage[MC_TX_RING_CAP];
    mc_ringbuf_t tx;
    uint8_t login_start[64];
    uint8_t frame[MC_COMMAND_TEXT_CAP + 16u];
    uint8_t out[256];
    char text[MC_COMMAND_TEXT_CAP];
    uint8_t position = 0xffu;
    size_t frame_len;
    size_t out_len;
    const uint8_t handshake_login[] = {
        0x0f, 0x00, 0x2f, 0x09, '1','2','7','.','0','.','0','.','1', 0x63, 0xdd, 0x02
    };
    const uint8_t username[] = { 'p', 'l', 'a', 'y', '\n', 'e', 'r' };

#if MC_PROTOCOL_COMPRESSION_ENABLE && MC_USE_PSRAM_COMPRESSED_MAP
    {
        static uint8_t compressed_arena[4096];
        ASSERT_TRUE(mc_world_compressed_init(compressed_arena, sizeof(compressed_arena)));
    }
#endif
    mc_ringbuf_init(&tx, tx_storage, sizeof(tx_storage));
    mc_server_init(&server);
    ASSERT_TRUE(mc_server_receive(&server, handshake_login, sizeof(handshake_login), &tx));
    frame_len = write_login_start_bytes_frame(username, sizeof(username), login_start, sizeof(login_start));
    ASSERT_TRUE(frame_len > 0u);
    ASSERT_TRUE(mc_server_receive(&server, login_start, frame_len, &tx));
    ASSERT_EQ(server.state, MC_CONN_PLAY);
    ASSERT_TRUE(finish_play_bootstrap(&server, &tx, 1u));
    (void)drain_ring(&tx, out, sizeof(out));

    frame_len = write_serverbound_chat_frame("hello", frame, sizeof(frame));
    ASSERT_TRUE(frame_len > 0u);
    ASSERT_TRUE(mc_server_receive(&server, frame, frame_len, &tx));

    out_len = drain_ring(&tx, out, sizeof(out));
    ASSERT_TRUE(out_len > 0u);
    ASSERT_EQ(clientbound_frame_len(out, out_len), out_len);
    ASSERT_EQ(count_packet_id(out, out_len, 0x02), 1);
    ASSERT_TRUE(read_clientbound_chat_text(out, out_len, text, sizeof(text), &position));
    ASSERT_EQ(position, 0u);
    ASSERT_STREQ(text, "<play\ner> hello");
    return 0;
}

static int test_play_bootstrap_sets_flying_observation_spawn(void)
{
    mc_server_t server;
    uint8_t tx_storage[MC_TX_RING_CAP];
    mc_ringbuf_t tx;
    uint8_t out[4096];
    size_t out_len;
    size_t pos = 0;
    uint8_t abilities_flags = 0u;
    double x = 0.0;
    double y = 0.0;
    double z = 0.0;
    int abilities_count = 0;
    int position_count = 0;

    mc_ringbuf_init(&tx, tx_storage, sizeof(tx_storage));
    ASSERT_TRUE(enter_play(&server, &tx));

    ASSERT_TRUE(mc_server_tick(&server, &tx));
    out_len = drain_ring(&tx, out, sizeof(out));
    ASSERT_TRUE(out_len > 0u);
    ASSERT_TRUE(server.player_y == 32.0);

    while (pos < out_len) {
        size_t frame_len = clientbound_frame_len(out + pos, out_len - pos);
        int32_t packet_id = -1;

        ASSERT_TRUE(frame_len > 0u);
        ASSERT_TRUE(read_clientbound_packet_id(out + pos, out_len - pos, &packet_id));
        if (packet_id == 0x39) {
            abilities_count++;
            ASSERT_TRUE(read_clientbound_abilities_flags(out + pos, out_len - pos, &abilities_flags));
        } else if (packet_id == 0x08) {
            position_count++;
            ASSERT_TRUE(read_clientbound_position(out + pos, out_len - pos, &x, &y, &z));
        }
        pos += frame_len;
    }

    ASSERT_EQ(abilities_count, 1);
    ASSERT_EQ(position_count, 1);
    ASSERT_EQ(abilities_flags, 0x0fu);
    ASSERT_TRUE(x == 0.5);
    ASSERT_TRUE(y == 32.0);
    ASSERT_TRUE(z == 0.5);
    return 0;
}

static int test_play_movement_packets_update_player_state(void)
{
    mc_server_t server;
    uint8_t tx_storage[MC_TX_RING_CAP];
    mc_ringbuf_t tx;
    uint8_t body[64];
    uint8_t frame[80];
    size_t len;
    mc_writer_t w;

    mc_ringbuf_init(&tx, tx_storage, sizeof(tx_storage));
    ASSERT_TRUE(enter_play(&server, &tx));
    ASSERT_TRUE(finish_play_bootstrap(&server, &tx, 1u));

    mc_writer_init(&w, body, sizeof(body));
    ASSERT_TRUE(mc_write_varint(&w, 0x03));
    ASSERT_TRUE(mc_write_bool(&w, 1));
    len = wrap_serverbound_play_body(body, w.len, frame, sizeof(frame));
    ASSERT_TRUE(len > 0u);
    ASSERT_TRUE(mc_server_receive(&server, frame, len, &tx));
    ASSERT_EQ(server.player_on_ground, 1u);

    mc_writer_init(&w, body, sizeof(body));
    ASSERT_TRUE(mc_write_varint(&w, 0x04));
    write_f64_be(body + w.len, 12.25);
    w.len += 8u;
    write_f64_be(body + w.len, 7.5);
    w.len += 8u;
    write_f64_be(body + w.len, -3.75);
    w.len += 8u;
    ASSERT_TRUE(mc_write_bool(&w, 0));
    len = wrap_serverbound_play_body(body, w.len, frame, sizeof(frame));
    ASSERT_TRUE(len > 0u);
    ASSERT_TRUE(mc_server_receive(&server, frame, len, &tx));
    ASSERT_TRUE(server.player_x == 12.25);
    ASSERT_TRUE(server.player_y == 7.5);
    ASSERT_TRUE(server.player_z == -3.75);
    ASSERT_EQ(server.player_on_ground, 0u);

    mc_writer_init(&w, body, sizeof(body));
    ASSERT_TRUE(mc_write_varint(&w, 0x05));
    write_f32_be(body + w.len, 90.0f);
    w.len += 4u;
    write_f32_be(body + w.len, 15.0f);
    w.len += 4u;
    ASSERT_TRUE(mc_write_bool(&w, 1));
    len = wrap_serverbound_play_body(body, w.len, frame, sizeof(frame));
    ASSERT_TRUE(len > 0u);
    ASSERT_TRUE(mc_server_receive(&server, frame, len, &tx));
    ASSERT_TRUE(server.player_yaw == 90.0f);
    ASSERT_TRUE(server.player_pitch == 15.0f);
    ASSERT_EQ(server.player_on_ground, 1u);

    mc_writer_init(&w, body, sizeof(body));
    ASSERT_TRUE(mc_write_varint(&w, 0x06));
    write_f64_be(body + w.len, -2.0);
    w.len += 8u;
    write_f64_be(body + w.len, 8.0);
    w.len += 8u;
    write_f64_be(body + w.len, 4.0);
    w.len += 8u;
    write_f32_be(body + w.len, 180.0f);
    w.len += 4u;
    write_f32_be(body + w.len, -10.0f);
    w.len += 4u;
    ASSERT_TRUE(mc_write_bool(&w, 0));
    len = wrap_serverbound_play_body(body, w.len, frame, sizeof(frame));
    ASSERT_TRUE(len > 0u);
    ASSERT_TRUE(mc_server_receive(&server, frame, len, &tx));
    ASSERT_TRUE(server.player_x == -2.0);
    ASSERT_TRUE(server.player_y == 8.0);
    ASSERT_TRUE(server.player_z == 4.0);
    ASSERT_TRUE(server.player_yaw == 180.0f);
    ASSERT_TRUE(server.player_pitch == -10.0f);
    ASSERT_EQ(server.player_on_ground, 0u);
    return 0;
}

static int write_known_player_state_body(uint8_t *body, size_t body_cap, size_t *body_len)
{
    mc_writer_t w;

    mc_writer_init(&w, body, body_cap);
    ASSERT_TRUE(mc_write_varint(&w, 0x06));
    ASSERT_TRUE(w.len + 33u <= body_cap);
    write_f64_be(body + w.len, 19.75);
    w.len += 8u;
    write_f64_be(body + w.len, -4.5);
    w.len += 8u;
    write_f64_be(body + w.len, 2.25);
    w.len += 8u;
    write_f32_be(body + w.len, 135.0f);
    w.len += 4u;
    write_f32_be(body + w.len, -30.0f);
    w.len += 4u;
    ASSERT_TRUE(mc_write_bool(&w, 1));
    *body_len = w.len;
    return 0;
}

static int write_malformed_movement_body(uint8_t packet_id,
                                         int with_extra_byte,
                                         uint8_t *body,
                                         size_t body_cap,
                                         size_t *body_len)
{
    mc_writer_t w;

    mc_writer_init(&w, body, body_cap);
    ASSERT_TRUE(mc_write_varint(&w, packet_id));

    switch (packet_id) {
    case 0x03:
        if (with_extra_byte) {
            ASSERT_TRUE(mc_write_bool(&w, 0));
        }
        break;
    case 0x04:
        write_f64_be(body + w.len, -9.0);
        w.len += 8u;
        write_f64_be(body + w.len, 3.0);
        w.len += 8u;
        if (with_extra_byte) {
            write_f64_be(body + w.len, 12.0);
            w.len += 8u;
            ASSERT_TRUE(mc_write_bool(&w, 0));
        }
        break;
    case 0x05:
        write_f32_be(body + w.len, 45.0f);
        w.len += 4u;
        if (with_extra_byte) {
            write_f32_be(body + w.len, 5.0f);
            w.len += 4u;
            ASSERT_TRUE(mc_write_bool(&w, 0));
        }
        break;
    case 0x06:
        write_f64_be(body + w.len, 8.0);
        w.len += 8u;
        write_f64_be(body + w.len, 9.0);
        w.len += 8u;
        write_f64_be(body + w.len, 10.0);
        w.len += 8u;
        write_f32_be(body + w.len, 20.0f);
        w.len += 4u;
        if (with_extra_byte) {
            write_f32_be(body + w.len, -5.0f);
            w.len += 4u;
            ASSERT_TRUE(mc_write_bool(&w, 0));
        }
        break;
    default:
        return 1;
    }

    if (with_extra_byte) {
        ASSERT_TRUE(w.len < body_cap);
        body[w.len++] = 0x99;
    }
    ASSERT_TRUE(w.len <= body_cap);
    *body_len = w.len;
    return 0;
}

static int test_play_malformed_movement_packets_preserve_player_state(void)
{
    static const uint8_t packet_ids[] = { 0x03, 0x04, 0x05, 0x06 };
    mc_server_t server;
    uint8_t tx_storage[MC_TX_RING_CAP];
    mc_ringbuf_t tx;
    uint8_t body[64];
    uint8_t frame[80];
    size_t body_len;
    size_t frame_len;
    player_state_snapshot_t snapshot;

    mc_ringbuf_init(&tx, tx_storage, sizeof(tx_storage));
    ASSERT_TRUE(enter_play(&server, &tx));
    ASSERT_TRUE(finish_play_bootstrap(&server, &tx, 1u));

    ASSERT_TRUE(!write_known_player_state_body(body, sizeof(body), &body_len));
    frame_len = wrap_serverbound_play_body(body, body_len, frame, sizeof(frame));
    ASSERT_TRUE(frame_len > 0u);
    ASSERT_TRUE(mc_server_receive(&server, frame, frame_len, &tx));
    snapshot = snapshot_player_state(&server);

    for (size_t i = 0; i < sizeof(packet_ids); i++) {
        ASSERT_TRUE(!write_malformed_movement_body(packet_ids[i], 1, body, sizeof(body), &body_len));
        frame_len = wrap_serverbound_play_body(body, body_len, frame, sizeof(frame));
        ASSERT_TRUE(frame_len > 0u);
        ASSERT_TRUE(!mc_server_receive(&server, frame, frame_len, &tx));
        ASSERT_TRUE(!assert_player_state_unchanged(&server, &snapshot));

        ASSERT_TRUE(!write_malformed_movement_body(packet_ids[i], 0, body, sizeof(body), &body_len));
        frame_len = wrap_serverbound_play_body(body, body_len, frame, sizeof(frame));
        ASSERT_TRUE(frame_len > 0u);
        ASSERT_TRUE(!mc_server_receive(&server, frame, frame_len, &tx));
        ASSERT_TRUE(!assert_player_state_unchanged(&server, &snapshot));
    }

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
    int saw_bootstrap_keepalive = 0;

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
        ASSERT_TRUE(compressed_packet.frame_len <= out_len);
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
        if (out_len > compressed_packet.frame_len) {
            int32_t keepalive_id = -1;
            ASSERT_TRUE(read_clientbound_keepalive_id(chunk_out + compressed_packet.frame_len,
                                                     out_len - compressed_packet.frame_len,
                                                     &keepalive_id));
            ASSERT_EQ(keepalive_id, 1);
            saw_bootstrap_keepalive = 1;
        }
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
        ASSERT_TRUE(compressed_packet.frame_len <= out_len);
        ASSERT_TRUE(mc_packet_get_compressed_body(compressed_packet.body,
                                                 compressed_packet.body_len,
                                                 &body,
                                                 &body_len,
                                                 &data_len));
        ASSERT_EQ(data_len, 0);
        ASSERT_TRUE(body_len > MC_CHUNK_SECTION_BYTES);
        ASSERT_EQ(count_packet_id(chunk_out, out_len, 0x21), 1);
        if (out_len > compressed_packet.frame_len) {
            int32_t keepalive_id = -1;
            ASSERT_TRUE(read_clientbound_keepalive_id(chunk_out + compressed_packet.frame_len,
                                                     out_len - compressed_packet.frame_len,
                                                     &keepalive_id));
            ASSERT_EQ(keepalive_id, 1);
            saw_bootstrap_keepalive = 1;
        }
    }
#else
    for (size_t i = 0; i < 9u; i++) {
        ASSERT_TRUE(mc_server_tick(&server, &tx));
        out_len = drain_ring(&tx, chunk_out, sizeof(chunk_out));
        ASSERT_TRUE(out_len > MC_CHUNK_SECTION_BYTES);
        ASSERT_EQ(count_packet_id(chunk_out, out_len, 0x21), 1);
        if (count_packet_id(chunk_out, out_len, 0x00) == 1) {
            saw_bootstrap_keepalive = 1;
        }
    }
#endif
    if (!saw_bootstrap_keepalive) {
        ASSERT_TRUE(mc_server_tick(&server, &tx));
        out_len = drain_ring(&tx, chunk_out, sizeof(chunk_out));
        ASSERT_TRUE(out_len > 0u);
        ASSERT_TRUE(read_clientbound_keepalive_id(chunk_out, out_len, &packet_id));
        ASSERT_EQ(packet_id, 1);
    }

    ASSERT_TRUE(!test_login_enables_compression_and_accepts_plain_compressed_serverbound());
    ASSERT_TRUE(!test_compressed_partial_frame_rejects_invalid_inner_packet_id());
    ASSERT_TRUE(!test_play_keepalive_queues_and_accepts_response());
    ASSERT_TRUE(!test_play_keepalive_unanswered_requests_keep_incrementing());
    ASSERT_TRUE(!test_play_keepalive_activity_is_not_required_for_liveness());
    ASSERT_TRUE(!test_play_keepalive_waits_for_bootstrap_complete());
    ASSERT_TRUE(!test_play_keepalive_rolls_over_to_positive_id());
    ASSERT_TRUE(!test_play_keepalive_enqueue_failure_preserves_next_id());
    ASSERT_TRUE(!test_play_keepalive_uses_new_id_each_interval());
    ASSERT_TRUE(!test_play_chat_echoes_to_self());
    ASSERT_TRUE(!test_play_chat_escapes_quote_and_backslash());
    ASSERT_TRUE(!test_play_chat_command_help_queues_chat_response());
    ASSERT_TRUE(!test_play_spawn_and_tp_commands_queue_position());
    ASSERT_TRUE(!test_play_time_command_queues_time_update());
    ASSERT_TRUE(!test_play_weather_command_queues_game_state());
    ASSERT_TRUE(!test_play_weather_clear_and_thunder_queue_expected_game_states());
    ASSERT_TRUE(!test_play_command_queue_failure_preserves_state_and_tx());
    ASSERT_TRUE(!test_play_chat_rejects_control_bytes());
    ASSERT_TRUE(!test_play_chat_escapes_username_control_bytes());
    ASSERT_TRUE(!test_play_bootstrap_sets_flying_observation_spawn());
    ASSERT_TRUE(!test_play_movement_packets_update_player_state());
    ASSERT_TRUE(!test_play_malformed_movement_packets_preserve_player_state());

    return 0;
}
