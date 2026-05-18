#include "mc_varint.h"

size_t mc_varint_encode(int32_t value, uint8_t *dst, size_t dst_cap)
{
    uint32_t v = (uint32_t)value;
    size_t written = 0;

    do {
        if (written >= dst_cap) {
            return 0;
        }
        uint8_t byte = (uint8_t)(v & 0x7Fu);
        v >>= 7;
        if (v != 0u) {
            byte |= 0x80u;
        }
        dst[written++] = byte;
    } while (v != 0u);

    return written;
}

int mc_varint_decode(const uint8_t *src, size_t src_len, int32_t *value, size_t *used)
{
    uint32_t result = 0;
    unsigned shift = 0;

    for (size_t i = 0; i < src_len && i < 5u; i++) {
        uint8_t byte = src[i];
        result |= (uint32_t)(byte & 0x7Fu) << shift;
        if ((byte & 0x80u) == 0u) {
            if (i == 4u && (byte & 0x70u) != 0u) {
                return 0;
            }
            *value = (int32_t)result;
            *used = i + 1u;
            return 1;
        }
        shift += 7u;
    }

    return 0;
}
