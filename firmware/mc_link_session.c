#include "mc_link_session.h"
#include "mc_log.h"

#include <string.h>

#define MC_LINK_FIRMWARE_SUPPORTED_RATE_MASK 0x03ffu

static void put_u16_le(uint8_t *dst, uint16_t value)
{
    dst[0] = (uint8_t)(value & 0xffu);
    dst[1] = (uint8_t)(value >> 8);
}

static uint16_t get_u16_le(const uint8_t *src)
{
    return (uint16_t)((uint16_t)src[0] | ((uint16_t)src[1] << 8));
}

static void log_parser_counters_if_changed(const mc_link_parser_t *parser,
                                           uint32_t crc_before,
                                           uint32_t length_before,
                                           uint32_t resync_before)
{
    if (parser->crc_error_count == crc_before &&
        parser->length_error_count == length_before &&
        parser->resync_count == resync_before) {
        return;
    }
    MC_LOGD("link parser counters crc=%u length=%u resync=%u buffered=%u discarding=%u",
            (unsigned int)parser->crc_error_count,
            (unsigned int)parser->length_error_count,
            (unsigned int)parser->resync_count,
            (unsigned int)parser->len,
            (unsigned int)parser->discarding);
}

static uint16_t min_u16(uint16_t a, uint16_t b)
{
    return a < b ? a : b;
}

static int queue_frame(mc_link_session_t *session,
                       mc_ringbuf_t *tx,
                       uint8_t type,
                       uint16_t seq,
                       uint16_t ack,
                       const uint8_t *payload,
                       size_t payload_len)
{
    uint8_t frame[MC_LINK_MAX_FRAME_LEN];
    size_t frame_len = 0;
    if (!mc_link_encode(type, seq, ack, payload, payload_len, frame, sizeof(frame), &frame_len)) {
        session->error_count++;
        return 0;
    }
    if (mc_ringbuf_free(tx) < frame_len) {
        session->error_count++;
        return 0;
    }
    return mc_ringbuf_write(tx, frame, frame_len) == frame_len;
}

static int queue_control_frame(mc_link_session_t *session,
                               uint8_t type,
                               uint16_t seq,
                               uint16_t ack,
                               const uint8_t *payload,
                               size_t payload_len)
{
    return queue_frame(session, &session->control_tx, type, seq, ack, payload, payload_len);
}

static int queue_data_frame(mc_link_session_t *session,
                            uint8_t type,
                            uint16_t seq,
                            uint16_t ack,
                            const uint8_t *payload,
                            size_t payload_len)
{
    return queue_frame(session, &session->tx, type, seq, ack, payload, payload_len);
}

static int queue_ready(mc_link_session_t *session)
{
    uint8_t payload[11];
    put_u16_le(payload, session->negotiated_payload);
    put_u16_le(payload + 2u, session->credit_cap);
    put_u16_le(payload + 4u, session->credit_cap);
    put_u16_le(payload + 6u, session->supported_rate_mask);
    payload[8] = session->active_rate_profile;
    put_u16_le(payload + 9u, 0u);
    return queue_control_frame(session, MC_LINK_READY, 0u, 0u, payload, sizeof(payload));
}

static uint16_t rx_free_credit(const mc_link_session_t *session)
{
    size_t free_len;
    if (session->rx == 0) {
        return 0u;
    }
    free_len = mc_ringbuf_free(session->rx);
    return free_len > 0xffffu ? 0xffffu : (uint16_t)free_len;
}

static int queue_ack_c2m(mc_link_session_t *session, uint16_t ack)
{
    uint8_t payload[2];
    put_u16_le(payload, rx_free_credit(session));
    if (!queue_control_frame(session, MC_LINK_ACK_C2M, 0u, ack, payload, sizeof(payload))) {
        return 0;
    }
    session->c2m_ack_frames++;
    return 1;
}

static int queue_error(mc_link_session_t *session, uint8_t code, uint8_t detail)
{
    uint8_t payload[2];
    payload[0] = code;
    payload[1] = detail;
    session->error_count++;
    return queue_control_frame(session, MC_LINK_ERROR, 0u, 0u, payload, sizeof(payload));
}

static int peek_ringbuf(mc_ringbuf_t *rb, uint8_t *dst, size_t len)
{
    for (size_t i = 0u; i < len; ++i) {
        if (!mc_ringbuf_peek(rb, i, &dst[i])) {
            return 0;
        }
    }
    return 1;
}

static void clear_active_tx(mc_link_session_t *session)
{
    session->active_tx_queue = MC_LINK_SESSION_TX_NONE;
    session->active_tx_remaining = 0u;
}

static void reset_capabilities(mc_link_session_t *session)
{
    session->negotiated_payload = MC_LINK_DEFAULT_PAYLOAD;
    session->credit_cap = MC_LINK_INITIAL_CREDIT;
    session->supported_rate_mask = MC_LINK_FIRMWARE_SUPPORTED_RATE_MASK;
    session->active_rate_profile = 0u;
}

static int next_frame_len(const mc_ringbuf_t *tx, size_t *frame_len)
{
    size_t len;
    if (tx == 0 || frame_len == 0) {
        return 0;
    }
    len = mc_ringbuf_len(tx);
    for (size_t i = 0u; i < len; ++i) {
        uint8_t byte;
        if (!mc_ringbuf_peek(tx, i, &byte)) {
            return 0;
        }
        if (byte == MC_LINK_DELIMITER) {
            *frame_len = i + 1u;
            return 1;
        }
    }
    return 0;
}

static void truncate_ringbuf(mc_ringbuf_t *rb, size_t keep_len)
{
    if (keep_len >= mc_ringbuf_len(rb)) {
        return;
    }
    rb->head = (rb->tail + keep_len) % rb->cap;
    rb->len = keep_len;
}

void mc_link_session_init(mc_link_session_t *session, mc_ringbuf_t *rx)
{
    memset(session, 0, sizeof(*session));
    mc_link_parser_init(&session->parser);
    session->rx = rx;
    mc_ringbuf_init(&session->tx, session->tx_storage, sizeof(session->tx_storage));
    mc_ringbuf_init(&session->control_tx, session->control_tx_storage, sizeof(session->control_tx_storage));
    reset_capabilities(session);
    session->ready = 1u;
}

static void reset_link_state_preserving_control_tx(mc_link_session_t *session)
{
    session->rx_seq_expected = 0u;
    session->tx_seq_next = 0u;
    session->ready = 1u;
    mc_link_parser_init(&session->parser);
    if (session->active_tx_queue == MC_LINK_SESSION_TX_DATA) {
        truncate_ringbuf(&session->tx, session->active_tx_remaining);
    } else {
        mc_ringbuf_init(&session->tx, session->tx_storage, sizeof(session->tx_storage));
    }
    if (session->rx != 0) {
        mc_ringbuf_drop(session->rx, mc_ringbuf_len(session->rx));
    }
}

static void reset_link_state(mc_link_session_t *session)
{
    reset_link_state_preserving_control_tx(session);
    if (session->active_tx_queue == MC_LINK_SESSION_TX_CONTROL) {
        truncate_ringbuf(&session->control_tx, session->active_tx_remaining);
    } else {
        mc_ringbuf_init(&session->control_tx, session->control_tx_storage, sizeof(session->control_tx_storage));
        if (session->active_tx_queue != MC_LINK_SESSION_TX_DATA) {
            clear_active_tx(session);
        }
    }
}

static int handle_hello(mc_link_session_t *session, const mc_link_frame_t *frame)
{
    uint16_t requested_payload;
    uint16_t requested_credit;
    uint16_t requested_rate_mask;
    uint16_t supported_rate_mask;
    if (frame->len != 6u) {
        return queue_error(session, MC_LINK_ERR_BAD_LENGTH, 0u);
    }
    requested_payload = get_u16_le(frame->payload);
    requested_credit = get_u16_le(frame->payload + 2u);
    requested_rate_mask = get_u16_le(frame->payload + 4u);
    if (requested_payload == 0u || requested_credit == 0u) {
        return queue_error(session, MC_LINK_ERR_BAD_LENGTH, 0u);
    }
    supported_rate_mask = (uint16_t)((requested_rate_mask & MC_LINK_FIRMWARE_SUPPORTED_RATE_MASK) | 1u);
    reset_link_state(session);
    session->negotiated_payload = min_u16(requested_payload, MC_LINK_DEFAULT_PAYLOAD);
    session->credit_cap = min_u16(requested_credit, MC_LINK_INITIAL_CREDIT);
    session->supported_rate_mask = supported_rate_mask;
    session->active_rate_profile = 0u;
    return queue_ready(session);
}

static int handle_reset(mc_link_session_t *session, const mc_link_frame_t *frame)
{
    reset_link_state(session);
    session->reset_requested = 1u;
    if (!queue_control_frame(session, MC_LINK_RESET_ACK, frame->seq, frame->ack, 0, 0)) {
        return 0;
    }
    return queue_ready(session);
}

static int handle_data_c2m(mc_link_session_t *session, const mc_link_frame_t *frame)
{
    if (!session->ready) {
        return queue_error(session, MC_LINK_ERR_PROTOCOL_STATE, frame->type);
    }
    if (frame->seq == (uint16_t)(session->rx_seq_expected - 1u)) {
        session->c2m_duplicate_frames++;
        return queue_ack_c2m(session, session->rx_seq_expected);
    }
    if (frame->seq != session->rx_seq_expected) {
        (void)queue_error(session, MC_LINK_ERR_SEQUENCE, (uint8_t)frame->seq);
        return 0;
    }
    if (frame->len == 0u || frame->len > session->negotiated_payload) {
        return queue_error(session, MC_LINK_ERR_BAD_LENGTH, (uint8_t)frame->len);
    }
    if (session->rx == 0 || mc_ringbuf_free(session->rx) < frame->len) {
        (void)queue_error(session, MC_LINK_ERR_RX_OVERFLOW, (uint8_t)frame->len);
        return 0;
    }
    if (mc_ringbuf_write(session->rx, frame->payload, frame->len) != frame->len) {
        (void)queue_error(session, MC_LINK_ERR_RX_OVERFLOW, (uint8_t)frame->len);
        return 0;
    }
    session->rx_seq_expected++;
    session->data_c2m_frames++;
    return queue_ack_c2m(session, session->rx_seq_expected);
}

static int handle_frame(mc_link_session_t *session, const mc_link_frame_t *frame)
{
    switch (frame->type) {
    case MC_LINK_HELLO:
        return handle_hello(session, frame);
    case MC_LINK_RESET:
        return handle_reset(session, frame);
    case MC_LINK_DATA_C2M:
        return handle_data_c2m(session, frame);
    case MC_LINK_RATE_PROBE:
    {
        uint16_t sleep_us = frame->len >= 2u ? get_u16_le(frame->payload) : 0u;
        uint16_t nonce = frame->len >= 4u ? get_u16_le(frame->payload + 2u) : 0u;
        int queued;
        (void)sleep_us;
        (void)nonce;
        MC_LOGD("rate probe seq=%u len=%u sleep_us=%u nonce=%u",
                (unsigned int)frame->seq,
                (unsigned int)frame->len,
                (unsigned int)sleep_us,
                (unsigned int)nonce);
        queued = queue_control_frame(session, MC_LINK_RATE_PROBE_ACK,
                                     frame->seq, frame->ack,
                                     frame->payload, frame->len);
        MC_LOGD("rate probe ack queued=%u control_free=%u",
                (unsigned int)(queued ? 1u : 0u),
                (unsigned int)mc_ringbuf_free(&session->control_tx));
        return queued;
    }
    case MC_LINK_PING:
        return queue_control_frame(session, MC_LINK_PONG, frame->seq, frame->ack, frame->payload, frame->len);
    default:
        return queue_error(session, MC_LINK_ERR_UNEXPECTED_TYPE, frame->type);
    }
}

int mc_link_session_receive_bytes(mc_link_session_t *session, const uint8_t *src, size_t len)
{
    mc_link_frame_t frame;
    mc_link_parse_result_t result;
    uint32_t crc_before;
    uint32_t length_before;
    uint32_t resync_before;
    if (session == 0 || (len > 0u && src == 0)) {
        return 0;
    }
    crc_before = session->parser.crc_error_count;
    length_before = session->parser.length_error_count;
    resync_before = session->parser.resync_count;
    result = mc_link_parser_feed(&session->parser, src, len, &frame);
    while (result == MC_LINK_PARSE_FRAME) {
        if (!handle_frame(session, &frame)) {
            log_parser_counters_if_changed(&session->parser,
                                           crc_before,
                                           length_before,
                                           resync_before);
            return 0;
        }
        result = mc_link_parser_feed(&session->parser, 0, 0, &frame);
    }
    log_parser_counters_if_changed(&session->parser,
                                   crc_before,
                                   length_before,
                                   resync_before);
    return result != MC_LINK_PARSE_ERROR;
}

void mc_link_session_credit_consumed(mc_link_session_t *session, size_t consumed_len)
{
    (void)consumed_len;
    if (session == 0) {
        return;
    }
    (void)queue_ack_c2m(session, 0u);
}

int mc_link_session_reset_after_error(mc_link_session_t *session, uint8_t code, uint8_t detail)
{
    if (session == 0) {
        return 0;
    }
    if (!queue_error(session, code, detail)) {
        return 0;
    }
    reset_link_state_preserving_control_tx(session);
    return queue_ready(session);
}

int mc_link_session_queue_server_tx(mc_link_session_t *session, mc_ringbuf_t *server_tx)
{
    uint8_t payload[MC_LINK_MAX_PAYLOAD];
    size_t max_payload;
    size_t n;
    if (session == 0 || server_tx == 0 || !session->ready) {
        return 0;
    }
    max_payload = session->negotiated_payload;
    if (max_payload == 0u || max_payload > sizeof(payload)) {
        max_payload = sizeof(payload);
    }
    n = mc_ringbuf_len(server_tx);
    if (n > max_payload) {
        n = max_payload;
    }
    if (n == 0u) {
        return 1;
    }
    if (!peek_ringbuf(server_tx, payload, n)) {
        return 0;
    }
    if (!queue_data_frame(session, MC_LINK_DATA_M2C, session->tx_seq_next, session->rx_seq_expected, payload, n)) {
        return 0;
    }
    mc_ringbuf_drop(server_tx, n);
    session->tx_seq_next++;
    session->data_m2c_frames++;
    return 1;
}

size_t mc_link_session_read_tx(mc_link_session_t *session, uint8_t *dst, size_t max_len)
{
    mc_ringbuf_t *active_tx;
    size_t to_read;
    if (session == 0 || dst == 0 || max_len == 0u) {
        return 0u;
    }
    if (session->active_tx_queue == MC_LINK_SESSION_TX_NONE) {
        size_t frame_len;
        if (next_frame_len(&session->control_tx, &frame_len)) {
            session->active_tx_queue = MC_LINK_SESSION_TX_CONTROL;
            session->active_tx_remaining = frame_len;
        } else if (next_frame_len(&session->tx, &frame_len)) {
            session->active_tx_queue = MC_LINK_SESSION_TX_DATA;
            session->active_tx_remaining = frame_len;
        } else {
            return 0u;
        }
    }

    if (session->active_tx_queue == MC_LINK_SESSION_TX_CONTROL) {
        active_tx = &session->control_tx;
    } else if (session->active_tx_queue == MC_LINK_SESSION_TX_DATA) {
        active_tx = &session->tx;
    } else {
        clear_active_tx(session);
        return 0u;
    }

    if (session->active_tx_remaining == 0u || mc_ringbuf_len(active_tx) == 0u) {
        clear_active_tx(session);
        return 0u;
    }
    to_read = session->active_tx_remaining;
    if (to_read > max_len) {
        to_read = max_len;
    }
    if (to_read > mc_ringbuf_len(active_tx)) {
        clear_active_tx(session);
        return 0u;
    }
    to_read = mc_ringbuf_read(active_tx, dst, to_read);
    session->active_tx_remaining -= to_read;
    if (session->active_tx_remaining == 0u) {
        clear_active_tx(session);
    }
    return to_read;
}

int mc_link_session_take_reset_requested(mc_link_session_t *session)
{
    if (session == 0) {
        return 0;
    }
    int requested = session->reset_requested != 0u;
    session->reset_requested = 0u;
    return requested;
}

void mc_link_session_drop_queued_data(mc_link_session_t *session)
{
    if (session == 0) {
        return;
    }
    if (session->active_tx_queue == MC_LINK_SESSION_TX_DATA) {
        truncate_ringbuf(&session->tx, session->active_tx_remaining);
        return;
    }
    mc_ringbuf_init(&session->tx, session->tx_storage, sizeof(session->tx_storage));
    if (session->active_tx_queue != MC_LINK_SESSION_TX_CONTROL) {
        clear_active_tx(session);
    }
}
