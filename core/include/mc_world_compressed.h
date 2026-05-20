#ifndef MC_WORLD_COMPRESSED_H
#define MC_WORLD_COMPRESSED_H

#include <stddef.h>
#include <stdint.h>
#include "mc_ringbuf.h"

typedef struct {
    int32_t chunk_x;
    int32_t chunk_z;
    uint32_t raw_body_len;
    uint32_t compressed_len;
    const uint8_t *compressed;
} mc_world_compressed_chunk_t;

int mc_world_compressed_init(void *arena, size_t arena_len);
int mc_world_compressed_ready(void);
size_t mc_world_compressed_chunk_count(void);
const mc_world_compressed_chunk_t *mc_world_compressed_chunk(size_t index);
int mc_world_queue_compressed_spawn_chunk(mc_ringbuf_t *tx, size_t index);

#endif
