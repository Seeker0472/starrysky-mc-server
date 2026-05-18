#include "mc_ringbuf.h"

void mc_ringbuf_init(mc_ringbuf_t *rb, uint8_t *storage, size_t cap)
{
    rb->data = storage;
    rb->cap = cap;
    rb->head = 0;
    rb->tail = 0;
    rb->len = 0;
}

size_t mc_ringbuf_len(const mc_ringbuf_t *rb)
{
    return rb->len;
}

size_t mc_ringbuf_free(const mc_ringbuf_t *rb)
{
    return rb->cap - rb->len;
}

int mc_ringbuf_push(mc_ringbuf_t *rb, uint8_t byte)
{
    if (rb->len == rb->cap) {
        return 0;
    }
    rb->data[rb->head] = byte;
    rb->head = (rb->head + 1u) % rb->cap;
    rb->len++;
    return 1;
}

int mc_ringbuf_pop(mc_ringbuf_t *rb, uint8_t *byte)
{
    if (rb->len == 0u) {
        return 0;
    }
    *byte = rb->data[rb->tail];
    rb->tail = (rb->tail + 1u) % rb->cap;
    rb->len--;
    return 1;
}

size_t mc_ringbuf_write(mc_ringbuf_t *rb, const uint8_t *src, size_t len)
{
    size_t written = 0;
    while (written < len && mc_ringbuf_push(rb, src[written])) {
        written++;
    }
    return written;
}

size_t mc_ringbuf_read(mc_ringbuf_t *rb, uint8_t *dst, size_t max_len)
{
    size_t read = 0;
    while (read < max_len && mc_ringbuf_pop(rb, &dst[read])) {
        read++;
    }
    return read;
}

int mc_ringbuf_peek(const mc_ringbuf_t *rb, size_t offset, uint8_t *byte)
{
    if (offset >= rb->len) {
        return 0;
    }
    *byte = rb->data[(rb->tail + offset) % rb->cap];
    return 1;
}

void mc_ringbuf_drop(mc_ringbuf_t *rb, size_t count)
{
    if (count > rb->len) {
        count = rb->len;
    }
    rb->tail = (rb->tail + count) % rb->cap;
    rb->len -= count;
}
