#ifndef MC_PACKET_H
#define MC_PACKET_H

#include <stddef.h>
#include <stdint.h>
#include "mc_config.h"

typedef struct {
    const uint8_t *body;
    size_t body_len;
    size_t frame_len;
} mc_packet_t;

typedef struct {
    uint8_t *buf;
    size_t cap;
    size_t len;
} mc_writer_t;

void mc_writer_init(mc_writer_t *w, uint8_t *buf, size_t cap);
int mc_write_u8(mc_writer_t *w, uint8_t value);
int mc_write_i8(mc_writer_t *w, int8_t value);
int mc_write_u16(mc_writer_t *w, uint16_t value);
int mc_write_i16(mc_writer_t *w, int16_t value);
int mc_write_i32(mc_writer_t *w, int32_t value);
int mc_write_i64(mc_writer_t *w, int64_t value);
int mc_write_f32(mc_writer_t *w, float value);
int mc_write_f64(mc_writer_t *w, double value);
int mc_write_bool(mc_writer_t *w, int value);
int mc_write_varint(mc_writer_t *w, int32_t value);
int mc_write_bytes(mc_writer_t *w, const uint8_t *src, size_t len);
int mc_write_string(mc_writer_t *w, const char *str);
size_t mc_packet_wrap(const uint8_t *body, size_t body_len, uint8_t *dst, size_t dst_cap);
int mc_packet_try_read(const uint8_t *src, size_t src_len, mc_packet_t *packet);

#endif
