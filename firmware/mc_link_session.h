#ifndef MC_LINK_SESSION_H
#define MC_LINK_SESSION_H

#include <stddef.h>
#include <stdint.h>
#include "mc_link.h"
#include "mc_ringbuf.h"

#define MC_LINK_SESSION_TX_CAP (MC_LINK_MAX_FRAME_LEN * 4u)
#define MC_LINK_SESSION_TX_NONE 0u
#define MC_LINK_SESSION_TX_DATA 1u
#define MC_LINK_SESSION_TX_CONTROL 2u

typedef struct {
    mc_link_parser_t parser;
    mc_ringbuf_t *rx;
    uint8_t tx_storage[MC_LINK_SESSION_TX_CAP];
    uint8_t control_tx_storage[MC_LINK_SESSION_TX_CAP];
    mc_ringbuf_t tx;
    mc_ringbuf_t control_tx;
    uint16_t rx_seq_expected;
    uint16_t tx_seq_next;
    uint16_t negotiated_payload;
    uint16_t credit_cap;
    uint16_t supported_rate_mask;
    uint8_t active_rate_profile;
    uint8_t ready;
    uint8_t reset_requested;
    uint8_t active_tx_queue;
    size_t active_tx_remaining;
    uint32_t data_c2m_frames;
    uint32_t data_m2c_frames;
    uint32_t error_count;
    uint32_t c2m_ack_frames;
    uint32_t c2m_duplicate_frames;
} mc_link_session_t;

void mc_link_session_init(mc_link_session_t *session, mc_ringbuf_t *rx);
int mc_link_session_receive_bytes(mc_link_session_t *session, const uint8_t *src, size_t len);
void mc_link_session_credit_consumed(mc_link_session_t *session, size_t consumed_len);
int mc_link_session_reset_after_error(mc_link_session_t *session, uint8_t code, uint8_t detail);
int mc_link_session_queue_server_tx(mc_link_session_t *session, mc_ringbuf_t *server_tx);
size_t mc_link_session_read_tx(mc_link_session_t *session, uint8_t *dst, size_t max_len);
int mc_link_session_take_reset_requested(mc_link_session_t *session);
void mc_link_session_drop_queued_data(mc_link_session_t *session);

#endif
