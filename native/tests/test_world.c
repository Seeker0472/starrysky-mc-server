#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include "mc_config.h"
#include "mc_packet.h"
#include "mc_ringbuf.h"
#include "mc_varint.h"
#include "mc_world.h"
#include "mc_world_compressed.h"
#if MC_USE_PSRAM_COMPRESSED_MAP
#include "mc_world_compressed_assets.h"
#endif

#define WORLD_TEST_TX_RING_CAP 131072u

#define ASSERT_TRUE(expr) do { if (!(expr)) return 1; } while (0)
#define ASSERT_EQ(a, b) do { if ((a) != (b)) return 1; } while (0)

static int read_varint(const uint8_t *body, size_t body_len, size_t *pos, int32_t *value)
{
    size_t used = 0;
    if (*pos > body_len || !mc_varint_decode(body + *pos, body_len - *pos, value, &used)) {
        return 0;
    }
    *pos += used;
    return 1;
}

static int read_i32(const uint8_t *body, size_t body_len, size_t *pos, int32_t *value)
{
    uint32_t v;
    if (*pos > body_len || body_len - *pos < 4u) {
        return 0;
    }
    v = ((uint32_t)body[*pos] << 24) |
        ((uint32_t)body[*pos + 1u] << 16) |
        ((uint32_t)body[*pos + 2u] << 8) |
        (uint32_t)body[*pos + 3u];
    *pos += 4u;
    *value = (int32_t)v;
    return 1;
}

static int read_u16(const uint8_t *body, size_t body_len, size_t *pos, uint16_t *value)
{
    if (*pos > body_len || body_len - *pos < 2u) {
        return 0;
    }
    *value = (uint16_t)(((uint16_t)body[*pos] << 8) | (uint16_t)body[*pos + 1u]);
    *pos += 2u;
    return 1;
}

static uint16_t section_block(const uint8_t *data, int x, int y, int z)
{
    size_t off = (size_t)(((y * 16) + z) * 16 + x) * 2u;
    return (uint16_t)((uint16_t)data[off] | ((uint16_t)data[off + 1u] << 8));
}

static int read_frame(mc_ringbuf_t *tx, uint8_t *frame, size_t frame_cap, size_t *frame_len)
{
    uint8_t prefix[5];
    int32_t body_len = 0;
    size_t prefix_len = 0;

    for (size_t i = 0; i < sizeof(prefix); i++) {
        ASSERT_TRUE(mc_ringbuf_peek(tx, i, &prefix[i]));
        if ((prefix[i] & 0x80u) == 0u) {
            ASSERT_TRUE(mc_varint_decode(prefix, i + 1u, &body_len, &prefix_len));
            ASSERT_TRUE(body_len >= 0);
            ASSERT_TRUE(prefix_len + (size_t)body_len <= frame_cap);
            ASSERT_EQ(mc_ringbuf_read(tx, frame, prefix_len + (size_t)body_len),
                      prefix_len + (size_t)body_len);
            *frame_len = prefix_len + (size_t)body_len;
            return 1;
        }
    }

    return 0;
}

static int check_first_chunk(const mc_packet_t *packet)
{
    size_t pos = 0;
    int32_t packet_id = 0;
    int32_t chunk_x = 0;
    int32_t chunk_z = 0;
    uint16_t mask = 0;
    int32_t data_size = 0;
    const uint8_t *data;

    ASSERT_TRUE(read_varint(packet->body, packet->body_len, &pos, &packet_id));
    ASSERT_EQ(packet_id, 0x21);
    ASSERT_TRUE(read_i32(packet->body, packet->body_len, &pos, &chunk_x));
    ASSERT_TRUE(read_i32(packet->body, packet->body_len, &pos, &chunk_z));
    ASSERT_EQ(chunk_x, 0);
    ASSERT_EQ(chunk_z, 0);
    ASSERT_TRUE(pos < packet->body_len);
    ASSERT_EQ(packet->body[pos++], 1u);
    ASSERT_TRUE(read_u16(packet->body, packet->body_len, &pos, &mask));
    ASSERT_EQ(mask, 0x0001u);
    ASSERT_TRUE(read_varint(packet->body, packet->body_len, &pos, &data_size));
    ASSERT_EQ(data_size, (int32_t)MC_CHUNK_SECTION_BYTES);
    ASSERT_EQ(packet->body_len - pos, MC_CHUNK_SECTION_BYTES);

    data = packet->body + pos;
    ASSERT_EQ(section_block(data, 0, 0, 0), (uint16_t)(7u << 4));
    ASSERT_EQ(section_block(data, 1, 1, 1), (uint16_t)(3u << 4));
    ASSERT_EQ(section_block(data, 2, 3, 2), (uint16_t)(3u << 4));
    ASSERT_EQ(section_block(data, 3, 4, 3), (uint16_t)(2u << 4));
    ASSERT_EQ(section_block(data, 4, 5, 4), 0u);
    ASSERT_EQ(data[8192u], 0xffu);
    ASSERT_EQ(data[8192u + 2047u], 0xffu);
    ASSERT_EQ(data[8192u + 2048u], 0xffu);
    ASSERT_EQ(data[8192u + 2048u + 2047u], 0xffu);
    ASSERT_EQ(data[8192u + 2048u + 2048u], 1u);
    ASSERT_EQ(data[MC_CHUNK_SECTION_BYTES - 1u], 1u);
    return 1;
}

static int read_chunk_header(const mc_packet_t *packet, int32_t *chunk_x, int32_t *chunk_z)
{
    size_t pos = 0;
    int32_t packet_id = 0;

    ASSERT_TRUE(read_varint(packet->body, packet->body_len, &pos, &packet_id));
    ASSERT_EQ(packet_id, 0x21);
    ASSERT_TRUE(read_i32(packet->body, packet->body_len, &pos, chunk_x));
    ASSERT_TRUE(read_i32(packet->body, packet->body_len, &pos, chunk_z));
    return 1;
}

static int test_compressed_world_init_and_queue(void)
{
#if MC_USE_PSRAM_COMPRESSED_MAP
    uint8_t arena[4096];
    uint8_t tx_storage[4096];
    uint8_t frame[4096];
    uint8_t data_len_varint[5];
    mc_ringbuf_t tx;
    const mc_world_compressed_chunk_t *chunk;
    size_t data_len_varint_len;
    size_t needed_len;
    size_t frame_len = 0;
    mc_packet_t packet;
    const uint8_t *compressed = 0;
    size_t compressed_len = 0;
    int32_t data_len = 0;

    ASSERT_TRUE(mc_world_compressed_asset_count == mc_world_spawn_chunk_count());
    ASSERT_TRUE(mc_world_compressed_total_bytes > 0u);
    ASSERT_TRUE(mc_world_compressed_total_bytes < sizeof(arena));
    ASSERT_TRUE(mc_world_compressed_init(arena, sizeof(arena)));
    ASSERT_TRUE(mc_world_compressed_ready());
    ASSERT_EQ(mc_world_compressed_chunk_count(), mc_world_compressed_asset_count);

    chunk = mc_world_compressed_chunk(0u);
    ASSERT_TRUE(chunk != 0);
    ASSERT_TRUE(chunk->compressed >= arena);
    ASSERT_TRUE(chunk->compressed + chunk->compressed_len <= arena + sizeof(arena));

    ASSERT_TRUE(!mc_world_compressed_init(arena, (size_t)mc_world_compressed_total_bytes - 1u));
    ASSERT_TRUE(!mc_world_compressed_ready());
    ASSERT_EQ(mc_world_compressed_chunk_count(), 0u);
    ASSERT_TRUE(mc_world_compressed_chunk(0u) == 0);

    ASSERT_TRUE(mc_world_compressed_init(arena, sizeof(arena)));
    ASSERT_TRUE(mc_world_compressed_ready());
    chunk = mc_world_compressed_chunk(0u);
    ASSERT_TRUE(chunk != 0);

    mc_ringbuf_init(&tx, tx_storage, sizeof(tx_storage));
    ASSERT_TRUE(mc_world_compressed_chunk(mc_world_compressed_chunk_count()) == 0);
    ASSERT_TRUE(!mc_world_queue_compressed_spawn_chunk(&tx, mc_world_compressed_chunk_count()));
    ASSERT_EQ(mc_ringbuf_len(&tx), 0u);

    needed_len = mc_packet_compressed_payload_frame_len((int32_t)chunk->raw_body_len, chunk->compressed_len);
    ASSERT_TRUE(needed_len > 1u);
    ASSERT_TRUE(needed_len <= sizeof(tx_storage));
    mc_ringbuf_init(&tx, tx_storage, needed_len - 1u);
    ASSERT_TRUE(!mc_world_queue_compressed_spawn_chunk(&tx, 0u));
    ASSERT_EQ(mc_ringbuf_len(&tx), 0u);

    mc_ringbuf_init(&tx, tx_storage, sizeof(tx_storage));
    ASSERT_TRUE(mc_world_queue_compressed_spawn_chunk(&tx, 0u));
    ASSERT_TRUE(read_frame(&tx, frame, sizeof(frame), &frame_len));
    ASSERT_EQ(frame_len, needed_len);
    ASSERT_TRUE(mc_packet_try_read(frame, frame_len, &packet));
    ASSERT_TRUE(mc_packet_get_compressed_body(packet.body, packet.body_len, &compressed, &compressed_len, &data_len));
    data_len_varint_len = mc_varint_encode((int32_t)chunk->raw_body_len,
                                           data_len_varint,
                                           sizeof(data_len_varint));
    ASSERT_TRUE(data_len_varint_len > 0u);
    ASSERT_EQ(data_len, (int32_t)chunk->raw_body_len);
    ASSERT_TRUE(data_len > (int32_t)MC_CHUNK_SECTION_BYTES);
    ASSERT_EQ(compressed_len, chunk->compressed_len);
    ASSERT_TRUE(compressed == packet.body + data_len_varint_len);
    ASSERT_TRUE(memcmp(compressed, chunk->compressed, compressed_len) == 0);
#endif
    return 0;
}

int test_world(void)
{
    static const int32_t expected_chunks[9][2] = {
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
    uint8_t tx_storage[WORLD_TEST_TX_RING_CAP];
    uint8_t frame[MC_MAX_PACKET_BODY + 8u];
    mc_ringbuf_t tx;
    mc_packet_t packet;
    size_t total_len;
    size_t frame_len = 0;

    mc_ringbuf_init(&tx, tx_storage, sizeof(tx_storage));
    ASSERT_TRUE(mc_world_queue_spawn_chunks(&tx));
    total_len = mc_ringbuf_len(&tx);
    ASSERT_TRUE(total_len > 9u * MC_CHUNK_SECTION_BYTES);

    for (size_t i = 0; i < 9u; i++) {
        ASSERT_TRUE(read_frame(&tx, frame, sizeof(frame), &frame_len));
        ASSERT_TRUE(mc_packet_try_read(frame, frame_len, &packet));
        ASSERT_EQ(packet.frame_len, frame_len);
        {
            int32_t chunk_x = 0;
            int32_t chunk_z = 0;
            ASSERT_TRUE(read_chunk_header(&packet, &chunk_x, &chunk_z));
            ASSERT_EQ(chunk_x, expected_chunks[i][0]);
            ASSERT_EQ(chunk_z, expected_chunks[i][1]);
        }
        if (i == 0u) {
            ASSERT_TRUE(check_first_chunk(&packet));
        }
    }
    ASSERT_EQ(mc_ringbuf_len(&tx), 0u);

    mc_ringbuf_init(&tx, tx_storage, 70000u);
    ASSERT_TRUE(!mc_world_queue_spawn_chunks(&tx));
    ASSERT_EQ(mc_ringbuf_len(&tx), 0u);

    ASSERT_TRUE(test_compressed_world_init_and_queue() == 0);

    return 0;
}
