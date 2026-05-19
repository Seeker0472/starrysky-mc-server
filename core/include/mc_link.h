#ifndef MC_LINK_H
#define MC_LINK_H

#include <stddef.h>
#include <stdint.h>

#define MC_LINK_VERSION 2u
#define MC_LINK_DELIMITER 0x00u
#define MC_LINK_BODY_HEADER_LEN 9u
#define MC_LINK_CRC_LEN 2u
#define MC_LINK_MAX_ENCODED_LEN 512u
#define MC_LINK_MAX_PAYLOAD 497u
#define MC_LINK_MAX_FRAME_LEN MC_LINK_MAX_ENCODED_LEN
#define MC_LINK_DEFAULT_PAYLOAD MC_LINK_MAX_PAYLOAD
#define MC_LINK_INITIAL_CREDIT 512u

typedef enum {
    MC_LINK_HELLO = 0x01u,
    MC_LINK_READY = 0x02u,
    MC_LINK_DATA_C2M = 0x03u,
    MC_LINK_ACK_C2M = 0x04u,
    MC_LINK_DATA_M2C = 0x05u,
    MC_LINK_RATE_PROBE = 0x06u,
    MC_LINK_RATE_PROBE_ACK = 0x07u,
    MC_LINK_RESET = 0x08u,
    MC_LINK_RESET_ACK = 0x09u,
    MC_LINK_ERROR = 0x0au,
    MC_LINK_PING = 0x0bu,
    MC_LINK_PONG = 0x0cu
} mc_link_type_t;

typedef enum {
    MC_LINK_ERR_BAD_VERSION = 1u,
    MC_LINK_ERR_BAD_LENGTH = 2u,
    MC_LINK_ERR_CRC_MISMATCH = 3u,
    MC_LINK_ERR_UNEXPECTED_TYPE = 4u,
    MC_LINK_ERR_SEQUENCE = 5u,
    MC_LINK_ERR_RX_OVERFLOW = 6u,
    MC_LINK_ERR_PROTOCOL_STATE = 7u
} mc_link_error_t;

typedef enum {
    MC_LINK_PARSE_NEED_MORE = 0,
    MC_LINK_PARSE_FRAME = 1,
    MC_LINK_PARSE_ERROR = 2
} mc_link_parse_result_t;

typedef struct {
    uint8_t type;
    uint8_t flags;
    uint16_t seq;
    uint16_t ack;
    uint16_t len;
    uint8_t payload[MC_LINK_MAX_PAYLOAD];
} mc_link_frame_t;

typedef struct {
    uint8_t buf[MC_LINK_MAX_FRAME_LEN];
    uint8_t decode_scratch[MC_LINK_BODY_HEADER_LEN + MC_LINK_MAX_PAYLOAD + MC_LINK_CRC_LEN];
    size_t len;
    uint32_t crc_error_count;
    uint32_t length_error_count;
    uint32_t resync_count;
    uint8_t discarding;
} mc_link_parser_t;

uint16_t mc_link_crc16(const uint8_t *src, size_t len);
int mc_link_encode(uint8_t type,
                   uint16_t seq,
                   uint16_t ack,
                   const uint8_t *payload,
                   size_t payload_len,
                   uint8_t *out,
                   size_t out_cap,
                   size_t *out_len);
int mc_link_decode_frame(const uint8_t *src,
                         size_t src_len,
                         mc_link_frame_t *frame);
void mc_link_parser_init(mc_link_parser_t *parser);
mc_link_parse_result_t mc_link_parser_feed(mc_link_parser_t *parser,
                                           const uint8_t *src,
                                           size_t len,
                                           mc_link_frame_t *frame);

#endif
