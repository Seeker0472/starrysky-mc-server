#include "mc_world_compressed.h"
#include "mc_config.h"

#if MC_USE_PSRAM_COMPRESSED_MAP
#include "mc_varint.h"
#include "mc_world_compressed_assets.h"
#include <limits.h>
#include <string.h>
#endif

#define MC_WORLD_COMPRESSED_MAX_CHUNKS 32u

#if MC_USE_PSRAM_COMPRESSED_MAP

static mc_world_compressed_chunk_t runtime_chunks[MC_WORLD_COMPRESSED_MAX_CHUNKS];
static size_t runtime_chunk_count;
static int runtime_ready;

static int compressed_chunk_frame_valid(uint32_t raw_body_len, uint32_t compressed_len)
{
    uint8_t data_len_varint[5];
    uint8_t prefix[5];
    size_t data_len_len;
    size_t inner_len;

    if (raw_body_len == 0u || raw_body_len > MC_MAX_PACKET_BODY ||
        compressed_len == 0u || compressed_len > MC_MAX_PACKET_BODY) {
        return 0;
    }

    data_len_len = mc_varint_encode((int32_t)raw_body_len,
                                    data_len_varint,
                                    sizeof(data_len_varint));
    if (data_len_len == 0u || compressed_len > (size_t)INT32_MAX - data_len_len) {
        return 0;
    }

    inner_len = data_len_len + compressed_len;
    if (inner_len > MC_MAX_PACKET_BODY ||
        mc_varint_encode((int32_t)inner_len, prefix, sizeof(prefix)) == 0u) {
        return 0;
    }

    return 1;
}

int mc_world_compressed_init(void *arena, size_t arena_len)
{
    uint8_t *dst = (uint8_t *)arena;
    size_t pos = 0;

    runtime_ready = 0;
    runtime_chunk_count = 0;

    if (arena == 0 || mc_world_compressed_asset_count > MC_WORLD_COMPRESSED_MAX_CHUNKS) {
        return 0;
    }
    if ((size_t)mc_world_compressed_total_bytes > arena_len) {
        return 0;
    }

    for (size_t i = 0; i < mc_world_compressed_asset_count; i++) {
        const mc_world_compressed_asset_t *asset = &mc_world_compressed_assets[i];
        if (asset->compressed == 0 || asset->compressed_len == 0u ||
            !compressed_chunk_frame_valid(asset->raw_body_len, asset->compressed_len) ||
            pos > arena_len ||
            asset->compressed_len > arena_len - pos) {
            return 0;
        }
        memcpy(dst + pos, asset->compressed, asset->compressed_len);
        runtime_chunks[i].chunk_x = asset->chunk_x;
        runtime_chunks[i].chunk_z = asset->chunk_z;
        runtime_chunks[i].raw_body_len = asset->raw_body_len;
        runtime_chunks[i].compressed_len = asset->compressed_len;
        runtime_chunks[i].compressed = dst + pos;
        pos += asset->compressed_len;
    }

    runtime_chunk_count = mc_world_compressed_asset_count;
    runtime_ready = 1;
    return 1;
}

int mc_world_compressed_ready(void)
{
    return runtime_ready;
}

size_t mc_world_compressed_chunk_count(void)
{
    return runtime_chunk_count;
}

const mc_world_compressed_chunk_t *mc_world_compressed_chunk(size_t index)
{
    if (!runtime_ready || index >= runtime_chunk_count) {
        return 0;
    }
    return &runtime_chunks[index];
}

int mc_world_queue_compressed_spawn_chunk(mc_ringbuf_t *tx, size_t index)
{
    const mc_world_compressed_chunk_t *chunk = mc_world_compressed_chunk(index);
    uint8_t prefix[5];
    uint8_t data_len_varint[5];
    size_t data_len_len;
    size_t inner_len;
    size_t prefix_len;
    size_t free_len;

    if (tx == 0 || chunk == 0) {
        return 0;
    }
    if (!compressed_chunk_frame_valid(chunk->raw_body_len, chunk->compressed_len)) {
        return 0;
    }
    data_len_len = mc_varint_encode((int32_t)chunk->raw_body_len, data_len_varint, sizeof(data_len_varint));
    if (data_len_len == 0u || chunk->compressed_len > (size_t)INT32_MAX - data_len_len) {
        return 0;
    }
    inner_len = data_len_len + chunk->compressed_len;
    prefix_len = mc_varint_encode((int32_t)inner_len, prefix, sizeof(prefix));
    free_len = mc_ringbuf_free(tx);
    if (prefix_len == 0u || free_len < prefix_len + inner_len) {
        return 0;
    }
    return mc_ringbuf_write(tx, prefix, prefix_len) == prefix_len &&
           mc_ringbuf_write(tx, data_len_varint, data_len_len) == data_len_len &&
           mc_ringbuf_write(tx, chunk->compressed, chunk->compressed_len) == chunk->compressed_len;
}

#else

int mc_world_compressed_init(void *arena, size_t arena_len)
{
    (void)arena;
    (void)arena_len;
    return 0;
}

int mc_world_compressed_ready(void)
{
    return 0;
}

size_t mc_world_compressed_chunk_count(void)
{
    return 0u;
}

const mc_world_compressed_chunk_t *mc_world_compressed_chunk(size_t index)
{
    (void)index;
    return 0;
}

int mc_world_queue_compressed_spawn_chunk(mc_ringbuf_t *tx, size_t index)
{
    (void)tx;
    (void)index;
    return 0;
}

#endif
