#ifndef MC_RINGBUF_H
#define MC_RINGBUF_H

#include <stddef.h>
#include <stdint.h>

typedef struct {
    uint8_t *data;
    size_t cap;
    size_t head;
    size_t tail;
    size_t len;
} mc_ringbuf_t;

void mc_ringbuf_init(mc_ringbuf_t *rb, uint8_t *storage, size_t cap);
size_t mc_ringbuf_len(const mc_ringbuf_t *rb);
size_t mc_ringbuf_free(const mc_ringbuf_t *rb);
int mc_ringbuf_push(mc_ringbuf_t *rb, uint8_t byte);
int mc_ringbuf_pop(mc_ringbuf_t *rb, uint8_t *byte);
size_t mc_ringbuf_write(mc_ringbuf_t *rb, const uint8_t *src, size_t len);
size_t mc_ringbuf_read(mc_ringbuf_t *rb, uint8_t *dst, size_t max_len);
int mc_ringbuf_peek(const mc_ringbuf_t *rb, size_t offset, uint8_t *byte);
void mc_ringbuf_drop(mc_ringbuf_t *rb, size_t count);

#endif
