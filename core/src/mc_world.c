#include "mc_world.h"
#include "mc_packet.h"
#include "mc_server.h"
#include "mc_varint.h"
#include <string.h>

#define MC_WORLD_CHUNK_COUNT 9u

/* Core world generation is single-thread/single-server oriented for firmware and native tests. */
#if !MC_USE_PSRAM_COMPRESSED_MAP
static uint8_t world_chunk_data[MC_CHUNK_SECTION_BYTES];
static uint8_t world_chunk_body[MC_CHUNK_SECTION_BYTES + 32u];

static const int32_t world_spawn_chunks[MC_WORLD_CHUNK_COUNT][2] = {
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

static uint16_t block_for_y(int y)
{
    if (y == 0) {
        return 7u << 4;
    }
    if (y <= 3) {
        return 3u << 4;
    }
    if (y == 4) {
        return 2u << 4;
    }
    return 0u;
}
#endif

size_t mc_world_spawn_chunk_count(void)
{
    return MC_WORLD_CHUNK_COUNT;
}

#if MC_USE_PSRAM_COMPRESSED_MAP
int mc_world_build_chunk_body(int32_t chunk_x, int32_t chunk_z, uint8_t *body, size_t body_cap, size_t *body_len)
{
    (void)chunk_x;
    (void)chunk_z;
    (void)body;
    (void)body_cap;
    if (body_len != 0) {
        *body_len = 0u;
    }
    return 0;
}

int mc_world_queue_spawn_chunk(mc_ringbuf_t *tx, size_t index)
{
    (void)tx;
    (void)index;
    return 0;
}

int mc_world_queue_spawn_chunks(mc_ringbuf_t *tx)
{
    (void)tx;
    return 0;
}
#else
int mc_world_build_chunk_body(int32_t chunk_x, int32_t chunk_z, uint8_t *body, size_t body_cap, size_t *body_len)
{
    mc_writer_t w;
    size_t pos = 0;

    if (body_len != NULL) {
        *body_len = 0u;
    }
    if (body == NULL || body_len == NULL) {
        return 0;
    }

    memset(world_chunk_data, 0, sizeof(world_chunk_data));
    for (int y = 0; y < 16; y++) {
        uint16_t block = block_for_y(y);
        for (int z = 0; z < 16; z++) {
            for (int x = 0; x < 16; x++) {
                size_t off = (size_t)(((y * 16) + z) * 16 + x) * 2u;
                world_chunk_data[off] = (uint8_t)block;
                world_chunk_data[off + 1u] = (uint8_t)(block >> 8);
            }
        }
    }

    pos = 8192u;
    memset(world_chunk_data + pos, 0xff, 2048u);
    pos += 2048u;
    memset(world_chunk_data + pos, 0xff, 2048u);
    pos += 2048u;
    memset(world_chunk_data + pos, 1u, 256u);

    mc_writer_init(&w, body, body_cap);
    if (!mc_write_varint(&w, 0x21) ||
        !mc_write_i32(&w, chunk_x) ||
        !mc_write_i32(&w, chunk_z) ||
        !mc_write_bool(&w, 1) ||
        !mc_write_u16(&w, 0x0001u) ||
        !mc_write_varint(&w, (int32_t)sizeof(world_chunk_data)) ||
        !mc_write_bytes(&w, world_chunk_data, sizeof(world_chunk_data))) {
        return 0;
    }

    *body_len = w.len;
    return 1;
}

int mc_world_queue_spawn_chunk(mc_ringbuf_t *tx, size_t index)
{
    size_t body_len = 0;

    if (tx == NULL || index >= MC_WORLD_CHUNK_COUNT) {
        return 0;
    }
    if (!mc_world_build_chunk_body(world_spawn_chunks[index][0],
                                   world_spawn_chunks[index][1],
                                   world_chunk_body,
                                   sizeof(world_chunk_body),
                                   &body_len)) {
        return 0;
    }
    return mc_server_queue_packet(tx, world_chunk_body, body_len);
}

static int preflight_spawn_chunk_space(mc_ringbuf_t *tx)
{
    size_t needed = 0;
    size_t free_len = mc_ringbuf_free(tx);

    for (size_t i = 0; i < MC_WORLD_CHUNK_COUNT; i++) {
        uint8_t prefix[5];
        size_t body_len = 0;
        size_t prefix_len;
        size_t frame_len;

        if (!mc_world_build_chunk_body(world_spawn_chunks[i][0],
                                       world_spawn_chunks[i][1],
                                       world_chunk_body,
                                       sizeof(world_chunk_body),
                                       &body_len)) {
            return 0;
        }
        prefix_len = mc_varint_encode((int32_t)body_len, prefix, sizeof(prefix));
        if (prefix_len == 0u || prefix_len > free_len || body_len > free_len - prefix_len) {
            return 0;
        }
        frame_len = prefix_len + body_len;
        if (needed > free_len || frame_len > free_len - needed) {
            return 0;
        }
        needed += frame_len;
    }

    return 1;
}

int mc_world_queue_spawn_chunks(mc_ringbuf_t *tx)
{
    if (tx == NULL) {
        return 0;
    }
    if (!preflight_spawn_chunk_space(tx)) {
        return 0;
    }

    for (size_t i = 0; i < MC_WORLD_CHUNK_COUNT; i++) {
        if (!mc_world_queue_spawn_chunk(tx, i)) {
            return 0;
        }
    }

    return 1;
}
#endif
