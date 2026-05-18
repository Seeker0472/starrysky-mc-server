#ifndef MC_WORLD_H
#define MC_WORLD_H

#include <stddef.h>
#include <stdint.h>
#include "mc_ringbuf.h"

#define MC_CHUNK_SECTION_BYTES 12544u

size_t mc_world_spawn_chunk_count(void);
int mc_world_build_chunk_body(int32_t chunk_x, int32_t chunk_z, uint8_t *body, size_t body_cap, size_t *body_len);
int mc_world_queue_spawn_chunk(mc_ringbuf_t *tx, size_t index);
int mc_world_queue_spawn_chunks(mc_ringbuf_t *tx);

#endif
