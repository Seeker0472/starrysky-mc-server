#include "mc_link_session.h"

#include <string.h>

static void put_u16_le(uint8_t *dst, uint16_t value)
{
    dst[0] = (uint8_t)(value & 0xffu);
    dst[1] = (uint8_t)(value >> 8);
}

static uint16_t get_u16_le(const uint8_t *src)
{
    return (uint16_t)((uint16_t)src[0] | ((uint16_t)src[1] << 8));
}

static uint16_t min_u16(uint16_t a, uint16_t b)
{
    return a < b ? a : b;
}

static int queue_frame(mc_link_session_t *session,
                       mc_ringbuf_t *tx,
                       uint8_t type,
                       uint8_t seq,
                       const uint8_t *payload,
                       size_t payload_len)
{
    uint8_t frame[MC_LINK_MAX_FRAME_LEN];
    size_t frame_len = 0;
    if (!mc_link_encode(type, seq, payload, payload_len, frame, sizeof(frame), &frame_len)) {
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
                               uint8_t seq,
                               const uint8_t *payload,
                               size_t payload_len)
{
    return queue_frame(session, &session->control_tx, type, seq, payload, payload_len);
}

static int queue_data_frame(mc_link_session_t *session,
                            uint8_t type,
                            uint8_t seq,
                            const uint8_t *payload,
                            size_t payload_len)
{
    return queue_frame(session, &session->tx, type, seq, payload, payload_len);
}

static int queue_ready(mc_link_session_t *session)
{
    uint8_t payload[5];
    payload[0] = MC_LINK_VERSION;
    put_u16_le(payload + 1, session->negotiated_payload);
    put_u16_le(payload + 3, session->credit_cap);
    return queue_control_frame(session, MC_LINK_READY, 0, payload, sizeof(payload));
}

static int queue_error(mc_link_session_t *session, uint8_t code, uint8_t detail)
{
    uint8_t payload[2];
    payload[0] = code;
    payload[1] = detail;
    session->error_count++;
    return queue_control_frame(session, MC_LINK_ERROR, 0, payload, sizeof(payload));
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

static int next_frame_len(const mc_ringbuf_t *tx, size_t *frame_len)
{
    uint8_t len_lo;
    uint8_t len_hi;
    size_t payload_len;
    if (mc_ringbuf_len(tx) < MC_LINK_HEADER_LEN) {
        return 0;
    }
    if (!mc_ringbuf_peek(tx, 4u, &len_lo) || !mc_ringbuf_peek(tx, 5u, &len_hi)) {
        return 0;
    }
    payload_len = (size_t)len_lo | ((size_t)len_hi << 8);
    if (payload_len > MC_LINK_FIRMWARE_PAYLOAD_CAP) {
        return 0;
    }
    *frame_len = MC_LINK_HEADER_LEN + payload_len + MC_LINK_CRC_LEN;
    return mc_ringbuf_len(tx) >= *frame_len;
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
    session->negotiated_payload = MC_LINK_DEFAULT_PAYLOAD;
    session->credit_cap = MC_LINK_INITIAL_CREDIT;
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
    if (frame->len != 5u || frame->payload[0] != MC_LINK_VERSION) {
        return queue_error(session,
                           frame->len == 5u ? MC_LINK_ERR_BAD_VERSION : MC_LINK_ERR_BAD_LENGTH,
                           frame->len == 5u ? frame->payload[0] : 0u);
    }
    requested_payload = get_u16_le(frame->payload + 1);
    requested_credit = get_u16_le(frame->payload + 3);
    if (requested_payload == 0u || requested_credit == 0u) {
        return queue_error(session, MC_LINK_ERR_BAD_LENGTH, 0);
    }
    session->negotiated_payload = min_u16(requested_payload, MC_LINK_FIRMWARE_PAYLOAD_CAP);
    session->credit_cap = min_u16(requested_credit, MC_LINK_INITIAL_CREDIT);
    reset_link_state(session);
    return queue_ready(session);
}

static int handle_reset(mc_link_session_t *session)
{
    reset_link_state(session);
    session->reset_requested = 1u;
    if (!queue_control_frame(session, MC_LINK_RESET_ACK, 0, 0, 0)) {
        return 0;
    }
    return queue_ready(session);
}

static int handle_data_c2m(mc_link_session_t *session, const mc_link_frame_t *frame)
{
    if (!session->ready) {
        return queue_error(session, MC_LINK_ERR_PROTOCOL_STATE, frame->type);
    }
    if (frame->seq != session->rx_seq_expected) {
        (void)queue_error(session, MC_LINK_ERR_SEQUENCE, frame->seq);
        reset_link_state_preserving_control_tx(session);
        return 0;
    }
    if (frame->len == 0u || frame->len > session->negotiated_payload) {
        return queue_error(session, MC_LINK_ERR_BAD_LENGTH, (uint8_t)frame->len);
    }
    if (session->rx == 0 || mc_ringbuf_free(session->rx) < frame->len) {
        (void)queue_error(session, MC_LINK_ERR_RX_OVERFLOW, (uint8_t)frame->len);
        reset_link_state_preserving_control_tx(session);
        return 0;
    }
    if (mc_ringbuf_write(session->rx, frame->payload, frame->len) != frame->len) {
        (void)queue_error(session, MC_LINK_ERR_RX_OVERFLOW, (uint8_t)frame->len);
        reset_link_state_preserving_control_tx(session);
        return 0;
    }
    session->rx_seq_expected++;
    session->data_c2m_frames++;
    return 1;
}

static int handle_frame(mc_link_session_t *session, const mc_link_frame_t *frame)
{
    switch (frame->type) {
    case MC_LINK_HELLO:
        return handle_hello(session, frame);
    case MC_LINK_RESET:
        return handle_reset(session);
    case MC_LINK_DATA_C2M:
        return handle_data_c2m(session, frame);
    case MC_LINK_PING:
        return queue_control_frame(session, MC_LINK_PONG, frame->seq, frame->payload, frame->len);
    default:
        return queue_error(session, MC_LINK_ERR_UNEXPECTED_TYPE, frame->type);
    }
}

int mc_link_session_receive_bytes(mc_link_session_t *session, const uint8_t *src, size_t len)
{
    mc_link_frame_t frame;
    mc_link_parse_result_t result;
    if (session == 0 || (len > 0u && src == 0)) {
        return 0;
    }
    result = mc_link_parser_feed(&session->parser, src, len, &frame);
    while (result == MC_LINK_PARSE_FRAME) {
        if (!handle_frame(session, &frame)) {
            return 0;
        }
        result = mc_link_parser_feed(&session->parser, 0, 0, &frame);
    }
    return result != MC_LINK_PARSE_ERROR;
}

void mc_link_session_credit_consumed(mc_link_session_t *session, size_t consumed_len)
{
    if (session == 0) {
        return;
    }
    while (consumed_len > 0u) {
        uint8_t payload[2];
        uint16_t chunk = consumed_len > 0xffffu ? 0xffffu : (uint16_t)consumed_len;
        put_u16_le(payload, chunk);
        if (!queue_control_frame(session, MC_LINK_CREDIT, 0, payload, sizeof(payload))) {
            return;
        }
        consumed_len -= chunk;
    }
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
    uint8_t payload[MC_LINK_FIRMWARE_PAYLOAD_CAP];
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
    if (!queue_data_frame(session, MC_LINK_DATA_M2C, session->tx_seq_next, payload, n)) {
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
