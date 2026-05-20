#include "mc_packet.h"
#include "mc_varint.h"
#include <limits.h>
#include <string.h>

static int mc_writer_has_space(const mc_writer_t *w, size_t len)
{
    return w->len <= w->cap && len <= w->cap - w->len;
}

static int mc_write_raw(mc_writer_t *w, uint8_t byte)
{
    if (!mc_writer_has_space(w, 1u)) {
        return 0;
    }
    w->buf[w->len++] = byte;
    return 1;
}

void mc_writer_init(mc_writer_t *w, uint8_t *buf, size_t cap)
{
    w->buf = buf;
    w->cap = cap;
    w->len = 0;
}

int mc_write_u8(mc_writer_t *w, uint8_t value) { return mc_write_raw(w, value); }
int mc_write_i8(mc_writer_t *w, int8_t value) { return mc_write_raw(w, (uint8_t)value); }

int mc_write_u16(mc_writer_t *w, uint16_t value)
{
    if (!mc_writer_has_space(w, 2u)) {
        return 0;
    }
    return mc_write_raw(w, (uint8_t)(value >> 8)) &&
           mc_write_raw(w, (uint8_t)value);
}

int mc_write_i16(mc_writer_t *w, int16_t value)
{
    return mc_write_u16(w, (uint16_t)value);
}

int mc_write_i32(mc_writer_t *w, int32_t value)
{
    uint32_t v = (uint32_t)value;
    if (!mc_writer_has_space(w, 4u)) {
        return 0;
    }
    return mc_write_raw(w, (uint8_t)(v >> 24)) &&
           mc_write_raw(w, (uint8_t)(v >> 16)) &&
           mc_write_raw(w, (uint8_t)(v >> 8)) &&
           mc_write_raw(w, (uint8_t)v);
}

int mc_write_i64(mc_writer_t *w, int64_t value)
{
    uint64_t v = (uint64_t)value;
    if (!mc_writer_has_space(w, 8u)) {
        return 0;
    }
    for (int shift = 56; shift >= 0; shift -= 8) {
        if (!mc_write_raw(w, (uint8_t)(v >> shift))) {
            return 0;
        }
    }
    return 1;
}

int mc_write_f32(mc_writer_t *w, float value)
{
    union { float f; uint32_t u; } u;
    u.f = value;
    return mc_write_i32(w, (int32_t)u.u);
}

int mc_write_f64(mc_writer_t *w, double value)
{
    union { double d; uint64_t u; } u;
    u.d = value;
    if (!mc_writer_has_space(w, 8u)) {
        return 0;
    }
    for (int shift = 56; shift >= 0; shift -= 8) {
        if (!mc_write_raw(w, (uint8_t)(u.u >> shift))) {
            return 0;
        }
    }
    return 1;
}

int mc_write_bool(mc_writer_t *w, int value)
{
    return mc_write_raw(w, value ? 1u : 0u);
}

int mc_write_varint(mc_writer_t *w, int32_t value)
{
    uint8_t tmp[5];
    size_t n = mc_varint_encode(value, tmp, sizeof(tmp));
    return n != 0u && mc_write_bytes(w, tmp, n);
}

int mc_write_bytes(mc_writer_t *w, const uint8_t *src, size_t len)
{
    if (!mc_writer_has_space(w, len)) {
        return 0;
    }
    memcpy(w->buf + w->len, src, len);
    w->len += len;
    return 1;
}

int mc_write_string(mc_writer_t *w, const char *str)
{
    size_t len = strlen(str);
    size_t start = w->len;
    if (len > MC_MAX_STRING_BYTES) {
        return 0;
    }
    if (!mc_write_varint(w, (int32_t)len) ||
        !mc_write_bytes(w, (const uint8_t *)str, len)) {
        w->len = start;
        return 0;
    }
    return 1;
}

size_t mc_packet_wrap(const uint8_t *body, size_t body_len, uint8_t *dst, size_t dst_cap)
{
    uint8_t prefix[5];
    size_t prefix_len = 0;
    if (body_len > MC_MAX_PACKET_BODY || body_len > (size_t)INT32_MAX) {
        return 0u;
    }
    prefix_len = mc_varint_encode((int32_t)body_len, prefix, sizeof(prefix));
    if (prefix_len == 0u || dst_cap < prefix_len || body_len > dst_cap - prefix_len) {
        return 0u;
    }
    memcpy(dst, prefix, prefix_len);
    memcpy(dst + prefix_len, body, body_len);
    return prefix_len + body_len;
}

size_t mc_packet_frame_len(size_t body_len)
{
    uint8_t prefix[5];
    size_t prefix_len;
    if (body_len > MC_MAX_PACKET_BODY || body_len > (size_t)INT32_MAX) {
        return 0u;
    }
    prefix_len = mc_varint_encode((int32_t)body_len, prefix, sizeof(prefix));
    if (prefix_len == 0u) {
        return 0u;
    }
    return prefix_len + body_len;
}

size_t mc_packet_compressed_plain_frame_len(size_t body_len)
{
    uint8_t outer[5];
    size_t inner_len = 1u + body_len;
    size_t outer_len = mc_varint_encode((int32_t)inner_len, outer, sizeof(outer));
    if (body_len > MC_MAX_PACKET_BODY || inner_len > (size_t)INT32_MAX || outer_len == 0u) {
        return 0u;
    }
    return outer_len + inner_len;
}

size_t mc_packet_compressed_payload_frame_len(int32_t uncompressed_body_len,
                                              size_t compressed_payload_len)
{
    uint8_t data_len_varint[5];
    uint8_t outer[5];
    size_t data_len_len;
    size_t inner_len;
    size_t outer_len;

    if (uncompressed_body_len <= 0 ||
        (size_t)uncompressed_body_len > MC_MAX_PACKET_BODY ||
        compressed_payload_len > MC_MAX_PACKET_BODY) {
        return 0u;
    }
    data_len_len = mc_varint_encode(uncompressed_body_len, data_len_varint, sizeof(data_len_varint));
    if (data_len_len == 0u || compressed_payload_len > (size_t)INT32_MAX - data_len_len) {
        return 0u;
    }
    inner_len = data_len_len + compressed_payload_len;
    outer_len = mc_varint_encode((int32_t)inner_len, outer, sizeof(outer));
    if (outer_len == 0u) {
        return 0u;
    }
    return outer_len + inner_len;
}

size_t mc_packet_wrap_compressed_plain(const uint8_t *body,
                                       size_t body_len,
                                       uint8_t *dst,
                                       size_t dst_cap)
{
    uint8_t outer[5];
    size_t inner_len = 1u + body_len;
    size_t outer_len;

    if (body == 0 || dst == 0 || body_len > MC_MAX_PACKET_BODY || inner_len > (size_t)INT32_MAX) {
        return 0u;
    }
    outer_len = mc_varint_encode((int32_t)inner_len, outer, sizeof(outer));
    if (outer_len == 0u || dst_cap < outer_len + inner_len) {
        return 0u;
    }
    memcpy(dst, outer, outer_len);
    dst[outer_len] = 0u;
    memcpy(dst + outer_len + 1u, body, body_len);
    return outer_len + inner_len;
}

size_t mc_packet_wrap_compressed_payload(int32_t uncompressed_body_len,
                                         const uint8_t *compressed_payload,
                                         size_t compressed_payload_len,
                                         uint8_t *dst,
                                         size_t dst_cap)
{
    uint8_t data_len_varint[5];
    uint8_t outer[5];
    size_t data_len_len;
    size_t inner_len;
    size_t outer_len;

    if (compressed_payload == 0 || dst == 0 ||
        uncompressed_body_len <= 0 ||
        (size_t)uncompressed_body_len > MC_MAX_PACKET_BODY ||
        compressed_payload_len > MC_MAX_PACKET_BODY) {
        return 0u;
    }
    data_len_len = mc_varint_encode(uncompressed_body_len, data_len_varint, sizeof(data_len_varint));
    if (data_len_len == 0u || compressed_payload_len > (size_t)INT32_MAX - data_len_len) {
        return 0u;
    }
    inner_len = data_len_len + compressed_payload_len;
    outer_len = mc_varint_encode((int32_t)inner_len, outer, sizeof(outer));
    if (outer_len == 0u || dst_cap < outer_len + inner_len) {
        return 0u;
    }
    memcpy(dst, outer, outer_len);
    memcpy(dst + outer_len, data_len_varint, data_len_len);
    memcpy(dst + outer_len + data_len_len, compressed_payload, compressed_payload_len);
    return outer_len + inner_len;
}

int mc_packet_get_compressed_body(const uint8_t *frame_body,
                                  size_t frame_body_len,
                                  const uint8_t **body,
                                  size_t *body_len,
                                  int32_t *data_len)
{
    size_t used = 0;
    int32_t decoded = 0;

    if (frame_body == 0 || body == 0 || body_len == 0 || data_len == 0) {
        return 0;
    }
    if (!mc_varint_decode(frame_body, frame_body_len, &decoded, &used) || used > frame_body_len) {
        return 0;
    }
    if (decoded < 0 || (size_t)decoded > MC_MAX_PACKET_BODY) {
        return 0;
    }
    *data_len = decoded;
    *body = frame_body + used;
    *body_len = frame_body_len - used;
    return 1;
}

int mc_packet_try_read(const uint8_t *src, size_t src_len, mc_packet_t *packet)
{
    int32_t body_len = 0;
    size_t prefix_len = 0;
    if (!mc_varint_decode(src, src_len, &body_len, &prefix_len)) {
        return 0;
    }
    if (body_len < 0 || (size_t)body_len > MC_MAX_PACKET_BODY) {
        return 0;
    }
    if (src_len < prefix_len + (size_t)body_len) {
        return 0;
    }
    packet->body = src + prefix_len;
    packet->body_len = (size_t)body_len;
    packet->frame_len = prefix_len + (size_t)body_len;
    return 1;
}
