#ifndef MC_VARINT_H
#define MC_VARINT_H

#include <stddef.h>
#include <stdint.h>

size_t mc_varint_encode(int32_t value, uint8_t *dst, size_t dst_cap);
int mc_varint_decode(const uint8_t *src, size_t src_len, int32_t *value, size_t *used);

#endif
