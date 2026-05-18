#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include "mc_link.h"
#include "mc_link_session.h"
#include "mc_ringbuf.h"

#define ASSERT_TRUE(expr) do { if (!(expr)) return 1; } while (0)
#define ASSERT_EQ(a, b) do { if ((a) != (b)) return 1; } while (0)
#define ASSERT_MEMEQ(a, b, n) do { if (memcmp((a), (b), (n)) != 0) return 1; } while (0)

void platform_init(void) {}

size_t platform_bridge_read(uint8_t *dst, size_t max_len)
{
    (void)dst;
    (void)max_len;
    return 0u;
}

size_t platform_bridge_write(const uint8_t *src, size_t max_len)
{
    (void)src;
    return max_len;
}

uint32_t platform_ticks(void)
{
    return 0u;
}

#define main firmware_main_entry
#include "../../firmware/main.c"
#undef main

static int read_frame_from_tx_chunked(mc_link_session_t *session, mc_link_frame_t *frame, size_t chunk_len)
{
    uint8_t out[MC_LINK_MAX_FRAME_LEN];
    mc_link_parser_t parser;
    mc_link_parser_init(&parser);
    for (;;) {
        size_t n = mc_link_session_read_tx(session, out, chunk_len);
        mc_link_parse_result_t result;
        if (n == 0u) return 0;
        result = mc_link_parser_feed(&parser, out, n, frame);
        if (result == MC_LINK_PARSE_FRAME) return 1;
        if (result == MC_LINK_PARSE_ERROR) return 0;
    }
}

static int read_frame_from_tx(mc_link_session_t *session, mc_link_frame_t *frame)
{
    return read_frame_from_tx_chunked(session, frame, 1u);
}

static int feed_encoded(mc_link_session_t *session, uint8_t type, uint8_t seq, const uint8_t *payload, size_t payload_len)
{
    uint8_t frame[MC_LINK_MAX_FRAME_LEN];
    size_t frame_len = 0;
    ASSERT_TRUE(mc_link_encode(type, seq, payload, payload_len, frame, sizeof(frame), &frame_len));
    ASSERT_TRUE(mc_link_session_receive_bytes(session, frame, frame_len));
    return 1;
}

static int feed_encoded_result(mc_link_session_t *session, uint8_t type, uint8_t seq, const uint8_t *payload, size_t payload_len)
{
    uint8_t frame[MC_LINK_MAX_FRAME_LEN];
    size_t frame_len = 0;
    ASSERT_TRUE(mc_link_encode(type, seq, payload, payload_len, frame, sizeof(frame), &frame_len));
    return mc_link_session_receive_bytes(session, frame, frame_len);
}

static int test_hello_emits_ready(void)
{
    mc_link_session_t session;
    mc_link_frame_t frame;
    uint8_t rx_storage[256];
    mc_ringbuf_t rx;
    uint8_t hello[] = {MC_LINK_VERSION, 0x00u, 0x02u, 0x00u, 0x02u};
    mc_ringbuf_init(&rx, rx_storage, sizeof(rx_storage));
    mc_link_session_init(&session, &rx);
    ASSERT_TRUE(feed_encoded(&session, MC_LINK_HELLO, 0, hello, sizeof(hello)));
    ASSERT_TRUE(read_frame_from_tx(&session, &frame));
    ASSERT_EQ(frame.type, MC_LINK_READY);
    ASSERT_EQ(frame.len, 5u);
    ASSERT_EQ(frame.payload[0], MC_LINK_VERSION);
    ASSERT_EQ((uint16_t)(frame.payload[1] | (frame.payload[2] << 8)), MC_LINK_DEFAULT_PAYLOAD);
    ASSERT_EQ((uint16_t)(frame.payload[3] | (frame.payload[4] << 8)), MC_LINK_INITIAL_CREDIT);
    return 0;
}

static int test_data_c2m_writes_rx_ring_and_sequence(void)
{
    mc_link_session_t session;
    uint8_t rx_storage[256];
    uint8_t out[8];
    mc_ringbuf_t rx;
    uint8_t hello[] = {MC_LINK_VERSION, 0x00u, 0x02u, 0x00u, 0x02u};
    uint8_t payload[] = {'h', 'i'};
    mc_ringbuf_init(&rx, rx_storage, sizeof(rx_storage));
    mc_link_session_init(&session, &rx);
    ASSERT_TRUE(feed_encoded(&session, MC_LINK_HELLO, 0, hello, sizeof(hello)));
    (void)mc_link_session_read_tx(&session, out, sizeof(out));
    ASSERT_TRUE(feed_encoded(&session, MC_LINK_DATA_C2M, 0, payload, sizeof(payload)));
    ASSERT_EQ(mc_ringbuf_read(&rx, out, sizeof(out)), 2u);
    ASSERT_MEMEQ(out, payload, sizeof(payload));
    return 0;
}

static int test_consumed_bytes_emit_credit(void)
{
    mc_link_session_t session;
    mc_link_frame_t frame;
    uint8_t rx_storage[256];
    mc_ringbuf_t rx;
    mc_ringbuf_init(&rx, rx_storage, sizeof(rx_storage));
    mc_link_session_init(&session, &rx);
    mc_link_session_credit_consumed(&session, 37u);
    ASSERT_TRUE(read_frame_from_tx(&session, &frame));
    ASSERT_EQ(frame.type, MC_LINK_CREDIT);
    ASSERT_EQ(frame.len, 2u);
    ASSERT_EQ((uint16_t)(frame.payload[0] | (frame.payload[1] << 8)), 37u);
    return 0;
}

static int test_wraps_server_tx_as_data_m2c(void)
{
    mc_link_session_t session;
    mc_link_frame_t frame;
    uint8_t rx_storage[256];
    uint8_t tx_storage[256];
    mc_ringbuf_t rx;
    mc_ringbuf_t server_tx;
    uint8_t payload[] = {'o', 'k'};
    mc_ringbuf_init(&rx, rx_storage, sizeof(rx_storage));
    mc_ringbuf_init(&server_tx, tx_storage, sizeof(tx_storage));
    mc_link_session_init(&session, &rx);
    ASSERT_EQ(mc_ringbuf_write(&server_tx, payload, sizeof(payload)), sizeof(payload));
    ASSERT_TRUE(mc_link_session_queue_server_tx(&session, &server_tx));
    ASSERT_TRUE(read_frame_from_tx(&session, &frame));
    ASSERT_EQ(frame.type, MC_LINK_DATA_M2C);
    ASSERT_EQ(frame.len, 2u);
    ASSERT_MEMEQ(frame.payload, payload, sizeof(payload));
    return 0;
}

static int test_reset_clears_rx_and_emits_ack_ready(void)
{
    mc_link_session_t session;
    mc_link_frame_t frame;
    uint8_t rx_storage[256];
    mc_ringbuf_t rx;
    uint8_t payload[] = {'x'};
    mc_ringbuf_init(&rx, rx_storage, sizeof(rx_storage));
    mc_link_session_init(&session, &rx);
    ASSERT_EQ(mc_ringbuf_write(&rx, payload, sizeof(payload)), 1u);
    ASSERT_TRUE(feed_encoded(&session, MC_LINK_RESET, 0, 0, 0));
    ASSERT_EQ(mc_ringbuf_len(&rx), 0u);
    ASSERT_TRUE(read_frame_from_tx(&session, &frame));
    ASSERT_EQ(frame.type, MC_LINK_RESET_ACK);
    ASSERT_TRUE(read_frame_from_tx(&session, &frame));
    ASSERT_EQ(frame.type, MC_LINK_READY);
    return 0;
}

static int test_sequence_mismatch_preserves_error_frame(void)
{
    mc_link_session_t session;
    mc_link_frame_t frame;
    uint8_t rx_storage[256];
    mc_ringbuf_t rx;
    uint8_t hello[] = {MC_LINK_VERSION, 0x00u, 0x02u, 0x00u, 0x02u};
    uint8_t first[] = {'a'};
    uint8_t skipped[] = {'b'};
    mc_ringbuf_init(&rx, rx_storage, sizeof(rx_storage));
    mc_link_session_init(&session, &rx);
    ASSERT_TRUE(feed_encoded(&session, MC_LINK_HELLO, 0, hello, sizeof(hello)));
    ASSERT_TRUE(read_frame_from_tx(&session, &frame));
    ASSERT_TRUE(feed_encoded(&session, MC_LINK_DATA_C2M, 0, first, sizeof(first)));
    ASSERT_TRUE(!feed_encoded_result(&session, MC_LINK_DATA_C2M, 2, skipped, sizeof(skipped)));
    ASSERT_TRUE(read_frame_from_tx(&session, &frame));
    ASSERT_EQ(frame.type, MC_LINK_ERROR);
    ASSERT_EQ(frame.len, 2u);
    ASSERT_EQ(frame.payload[0], MC_LINK_ERR_SEQUENCE);
    return 0;
}

static int test_read_tx_supports_partial_frame_reads(void)
{
    mc_link_session_t session;
    mc_link_frame_t frame;
    uint8_t rx_storage[256];
    uint8_t tx_storage[MC_LINK_FIRMWARE_PAYLOAD_CAP];
    uint8_t payload[MC_LINK_FIRMWARE_PAYLOAD_CAP];
    uint8_t encoded[MC_LINK_MAX_FRAME_LEN];
    uint8_t chunk[256];
    mc_ringbuf_t rx;
    mc_ringbuf_t server_tx;
    mc_link_parser_t parser;
    mc_link_parse_result_t result = MC_LINK_PARSE_NEED_MORE;
    size_t total = 0u;
    for (size_t i = 0u; i < sizeof(payload); ++i) {
        payload[i] = (uint8_t)i;
    }
    mc_ringbuf_init(&rx, rx_storage, sizeof(rx_storage));
    mc_ringbuf_init(&server_tx, tx_storage, sizeof(tx_storage));
    mc_link_session_init(&session, &rx);
    session.negotiated_payload = MC_LINK_FIRMWARE_PAYLOAD_CAP;
    ASSERT_EQ(mc_ringbuf_write(&server_tx, payload, sizeof(payload)), sizeof(payload));
    ASSERT_TRUE(mc_link_session_queue_server_tx(&session, &server_tx));
    mc_link_parser_init(&parser);
    for (;;) {
        size_t n = mc_link_session_read_tx(&session, chunk, sizeof(chunk));
        if (n == 0u) break;
        ASSERT_TRUE(total + n <= sizeof(encoded));
        memcpy(encoded + total, chunk, n);
        total += n;
        result = mc_link_parser_feed(&parser, chunk, n, &frame);
        if (result == MC_LINK_PARSE_FRAME) break;
        ASSERT_EQ(result, MC_LINK_PARSE_NEED_MORE);
    }
    ASSERT_TRUE(total > 0u);
    ASSERT_EQ(total, MC_LINK_MAX_FRAME_LEN);
    ASSERT_EQ(result, MC_LINK_PARSE_FRAME);
    ASSERT_EQ(frame.type, MC_LINK_DATA_M2C);
    ASSERT_EQ(frame.len, MC_LINK_FIRMWARE_PAYLOAD_CAP);
    ASSERT_MEMEQ(frame.payload, payload, sizeof(payload));
    ASSERT_MEMEQ(encoded + MC_LINK_HEADER_LEN, payload, sizeof(payload));
    return 0;
}

static int test_queue_server_tx_failure_preserves_server_tx(void)
{
    mc_link_session_t session;
    uint8_t rx_storage[256];
    uint8_t tx_storage[MC_LINK_FIRMWARE_PAYLOAD_CAP];
    uint8_t filler_payload[MC_LINK_FIRMWARE_PAYLOAD_CAP];
    uint8_t payload[] = {'k', 'e', 'e', 'p'};
    uint8_t out[MC_LINK_MAX_FRAME_LEN];
    mc_ringbuf_t rx;
    mc_ringbuf_t server_tx;
    mc_ringbuf_init(&rx, rx_storage, sizeof(rx_storage));
    mc_ringbuf_init(&server_tx, tx_storage, sizeof(tx_storage));
    mc_link_session_init(&session, &rx);
    session.negotiated_payload = MC_LINK_FIRMWARE_PAYLOAD_CAP;
    memset(filler_payload, 0xa5, sizeof(filler_payload));
    for (size_t i = 0u; i < 4u; ++i) {
        ASSERT_EQ(mc_ringbuf_write(&server_tx, filler_payload, sizeof(filler_payload)), sizeof(filler_payload));
        ASSERT_TRUE(mc_link_session_queue_server_tx(&session, &server_tx));
        ASSERT_EQ(mc_ringbuf_len(&server_tx), 0u);
    }
    ASSERT_EQ(mc_ringbuf_write(&server_tx, payload, sizeof(payload)), sizeof(payload));
    ASSERT_EQ(mc_ringbuf_len(&server_tx), sizeof(payload));
    ASSERT_TRUE(!mc_link_session_queue_server_tx(&session, &server_tx));
    ASSERT_EQ(mc_ringbuf_len(&server_tx), sizeof(payload));
    ASSERT_EQ(mc_ringbuf_read(&server_tx, out, sizeof(out)), sizeof(payload));
    ASSERT_MEMEQ(out, payload, sizeof(payload));
    return 0;
}

static int test_control_frames_precede_backed_up_data(void)
{
    mc_link_session_t session;
    mc_link_frame_t frame;
    uint8_t rx_storage[256];
    uint8_t tx_storage[MC_LINK_FIRMWARE_PAYLOAD_CAP];
    uint8_t filler_payload[MC_LINK_FIRMWARE_PAYLOAD_CAP];
    mc_ringbuf_t rx;
    mc_ringbuf_t server_tx;
    mc_ringbuf_init(&rx, rx_storage, sizeof(rx_storage));
    mc_ringbuf_init(&server_tx, tx_storage, sizeof(tx_storage));
    mc_link_session_init(&session, &rx);
    session.negotiated_payload = MC_LINK_FIRMWARE_PAYLOAD_CAP;
    memset(filler_payload, 0xa5, sizeof(filler_payload));
    for (size_t i = 0u; i < 4u; ++i) {
        ASSERT_EQ(mc_ringbuf_write(&server_tx, filler_payload, sizeof(filler_payload)), sizeof(filler_payload));
        ASSERT_TRUE(mc_link_session_queue_server_tx(&session, &server_tx));
        ASSERT_EQ(mc_ringbuf_len(&server_tx), 0u);
    }
    mc_link_session_credit_consumed(&session, 1u);
    ASSERT_TRUE(read_frame_from_tx(&session, &frame));
    ASSERT_EQ(frame.type, MC_LINK_CREDIT);
    ASSERT_EQ(frame.len, 2u);
    ASSERT_EQ((uint16_t)(frame.payload[0] | (frame.payload[1] << 8)), 1u);
    return 0;
}

static int test_malformed_zero_length_hello_reports_bad_length(void)
{
    mc_link_session_t session;
    mc_link_frame_t frame;
    uint8_t rx_storage[256];
    mc_ringbuf_t rx;
    mc_ringbuf_init(&rx, rx_storage, sizeof(rx_storage));
    mc_link_session_init(&session, &rx);
    ASSERT_TRUE(feed_encoded(&session, MC_LINK_HELLO, 0, 0, 0));
    ASSERT_TRUE(read_frame_from_tx(&session, &frame));
    ASSERT_EQ(frame.type, MC_LINK_ERROR);
    ASSERT_EQ(frame.len, 2u);
    ASSERT_EQ(frame.payload[0], MC_LINK_ERR_BAD_LENGTH);
    ASSERT_EQ(frame.payload[1], 0u);
    return 0;
}

static int test_null_public_api_guards(void)
{
    mc_link_session_credit_consumed(0, 1u);
    mc_link_session_drop_queued_data(0);
    ASSERT_EQ(mc_link_session_take_reset_requested(0), 0);
    return 0;
}

static int test_control_waits_for_active_data_frame_boundary(void)
{
    mc_link_session_t session;
    mc_link_frame_t frame;
    uint8_t rx_storage[256];
    uint8_t tx_storage[MC_LINK_FIRMWARE_PAYLOAD_CAP];
    uint8_t payload[MC_LINK_FIRMWARE_PAYLOAD_CAP];
    uint8_t chunk[64];
    mc_ringbuf_t rx;
    mc_ringbuf_t server_tx;
    mc_link_parser_t parser;
    mc_link_parse_result_t result;
    int saw_data_frame = 0;
    for (size_t i = 0u; i < sizeof(payload); ++i) {
        payload[i] = (uint8_t)(0xffu - i);
    }
    mc_ringbuf_init(&rx, rx_storage, sizeof(rx_storage));
    mc_ringbuf_init(&server_tx, tx_storage, sizeof(tx_storage));
    mc_link_session_init(&session, &rx);
    session.negotiated_payload = MC_LINK_FIRMWARE_PAYLOAD_CAP;
    ASSERT_EQ(mc_ringbuf_write(&server_tx, payload, sizeof(payload)), sizeof(payload));
    ASSERT_TRUE(mc_link_session_queue_server_tx(&session, &server_tx));
    mc_link_parser_init(&parser);
    ASSERT_EQ(mc_link_session_read_tx(&session, chunk, sizeof(chunk)), sizeof(chunk));
    ASSERT_EQ(mc_link_parser_feed(&parser, chunk, sizeof(chunk), &frame), MC_LINK_PARSE_NEED_MORE);
    mc_link_session_credit_consumed(&session, 1u);
    for (;;) {
        size_t n = mc_link_session_read_tx(&session, chunk, sizeof(chunk));
        if (n == 0u) break;
        result = mc_link_parser_feed(&parser, chunk, n, &frame);
        if (result == MC_LINK_PARSE_FRAME) {
            ASSERT_EQ(frame.type, MC_LINK_DATA_M2C);
            ASSERT_EQ(frame.len, MC_LINK_FIRMWARE_PAYLOAD_CAP);
            ASSERT_MEMEQ(frame.payload, payload, sizeof(payload));
            saw_data_frame = 1;
            break;
        }
        ASSERT_EQ(result, MC_LINK_PARSE_NEED_MORE);
    }
    ASSERT_TRUE(saw_data_frame);
    result = mc_link_parser_feed(&parser, 0, 0, &frame);
    while (result == MC_LINK_PARSE_NEED_MORE) {
        size_t n = mc_link_session_read_tx(&session, chunk, sizeof(chunk));
        ASSERT_TRUE(n > 0u);
        result = mc_link_parser_feed(&parser, chunk, n, &frame);
    }
    ASSERT_EQ(result, MC_LINK_PARSE_FRAME);
    ASSERT_EQ(frame.type, MC_LINK_CREDIT);
    ASSERT_EQ(frame.len, 2u);
    ASSERT_EQ((uint16_t)(frame.payload[0] | (frame.payload[1] << 8)), 1u);
    return 0;
}

static int test_drop_queued_data_without_active_data_discards_data_m2c(void)
{
    mc_link_session_t session;
    mc_link_frame_t frame;
    uint8_t rx_storage[256];
    uint8_t tx_storage[256];
    mc_ringbuf_t rx;
    mc_ringbuf_t server_tx;
    uint8_t first[] = {'o', 'l', 'd'};
    uint8_t second[] = {'n', 'e', 'w'};
    mc_ringbuf_init(&rx, rx_storage, sizeof(rx_storage));
    mc_ringbuf_init(&server_tx, tx_storage, sizeof(tx_storage));
    mc_link_session_init(&session, &rx);
    ASSERT_EQ(mc_ringbuf_write(&server_tx, first, sizeof(first)), sizeof(first));
    ASSERT_TRUE(mc_link_session_queue_server_tx(&session, &server_tx));
    ASSERT_EQ(mc_ringbuf_write(&server_tx, second, sizeof(second)), sizeof(second));
    ASSERT_TRUE(mc_link_session_queue_server_tx(&session, &server_tx));
    mc_link_session_drop_queued_data(&session);
    ASSERT_TRUE(!read_frame_from_tx(&session, &frame));
    return 0;
}

static int test_drop_queued_data_preserves_active_data_frame_tail(void)
{
    mc_link_session_t session;
    mc_link_frame_t frame;
    uint8_t rx_storage[256];
    uint8_t tx_storage[MC_LINK_FIRMWARE_PAYLOAD_CAP + 16u];
    uint8_t first[MC_LINK_FIRMWARE_PAYLOAD_CAP];
    uint8_t second[] = {'s', 't', 'a', 'l', 'e'};
    uint8_t chunk[17];
    mc_ringbuf_t rx;
    mc_ringbuf_t server_tx;
    mc_link_parser_t parser;
    mc_link_parse_result_t result;
    int saw_first = 0;
    for (size_t i = 0u; i < sizeof(first); ++i) {
        first[i] = (uint8_t)(i ^ 0x5au);
    }
    mc_ringbuf_init(&rx, rx_storage, sizeof(rx_storage));
    mc_ringbuf_init(&server_tx, tx_storage, sizeof(tx_storage));
    mc_link_session_init(&session, &rx);
    session.negotiated_payload = MC_LINK_FIRMWARE_PAYLOAD_CAP;
    ASSERT_EQ(mc_ringbuf_write(&server_tx, first, sizeof(first)), sizeof(first));
    ASSERT_TRUE(mc_link_session_queue_server_tx(&session, &server_tx));
    ASSERT_EQ(mc_ringbuf_write(&server_tx, second, sizeof(second)), sizeof(second));
    ASSERT_TRUE(mc_link_session_queue_server_tx(&session, &server_tx));
    mc_link_parser_init(&parser);
    ASSERT_EQ(mc_link_session_read_tx(&session, chunk, sizeof(chunk)), sizeof(chunk));
    ASSERT_EQ(mc_link_parser_feed(&parser, chunk, sizeof(chunk), &frame), MC_LINK_PARSE_NEED_MORE);
    mc_link_session_drop_queued_data(&session);
    for (;;) {
        size_t n = mc_link_session_read_tx(&session, chunk, sizeof(chunk));
        if (n == 0u) break;
        result = mc_link_parser_feed(&parser, chunk, n, &frame);
        if (result == MC_LINK_PARSE_FRAME) {
            ASSERT_EQ(frame.type, MC_LINK_DATA_M2C);
            ASSERT_EQ(frame.len, MC_LINK_FIRMWARE_PAYLOAD_CAP);
            ASSERT_MEMEQ(frame.payload, first, sizeof(first));
            saw_first = 1;
            break;
        }
        ASSERT_EQ(result, MC_LINK_PARSE_NEED_MORE);
    }
    ASSERT_TRUE(saw_first);
    ASSERT_TRUE(!read_frame_from_tx(&session, &frame));
    return 0;
}

static int feed_main_data_c2m(uint8_t seq, const uint8_t *payload, size_t payload_len)
{
    uint8_t encoded[MC_LINK_MAX_FRAME_LEN];
    size_t encoded_len = 0u;
    ASSERT_TRUE(mc_link_encode(MC_LINK_DATA_C2M,
                               seq,
                               payload,
                               payload_len,
                               encoded,
                               sizeof(encoded),
                               &encoded_len));
    ASSERT_TRUE(mc_link_session_receive_bytes(&link_session, encoded, encoded_len));
    return 1;
}

static int read_main_link_frame(mc_link_frame_t *frame)
{
    uint8_t chunk[MC_LINK_MAX_FRAME_LEN];
    mc_link_parser_t parser;
    mc_link_parse_result_t result = MC_LINK_PARSE_NEED_MORE;

    mc_link_parser_init(&parser);
    while (result == MC_LINK_PARSE_NEED_MORE) {
        size_t n = mc_link_session_read_tx(&link_session, chunk, sizeof(chunk));
        if (n == 0u) {
            return 0;
        }
        result = mc_link_parser_feed(&parser, chunk, n, frame);
    }

    return result == MC_LINK_PARSE_FRAME;
}

int test_firmware_main(void)
{
    mc_link_frame_t frame;
    const uint8_t invalid_varint_prefix[] = {0x80u, 0x80u, 0x80u, 0x80u, 0x80u, 0x00u};

    mc_link_session_init(&link_session, &rx_ring);
    reset_connection_quiet();

    ASSERT_TRUE(feed_main_data_c2m(0, invalid_varint_prefix, sizeof(invalid_varint_prefix)));
    ASSERT_EQ(link_session.rx_seq_expected, 1u);
    link_session.tx_seq_next = 9u;

    pump_server();

    ASSERT_EQ(mc_ringbuf_len(&rx_ring), 0u);
    ASSERT_EQ(server.state, MC_CONN_HANDSHAKE);
    ASSERT_EQ(link_session.rx_seq_expected, 0u);
    ASSERT_EQ(link_session.tx_seq_next, 0u);

    ASSERT_TRUE(read_main_link_frame(&frame));
    ASSERT_EQ(frame.type, MC_LINK_ERROR);
    ASSERT_EQ(frame.len, 2u);
    ASSERT_EQ(frame.payload[0], MC_LINK_ERR_PROTOCOL_STATE);

    ASSERT_TRUE(read_main_link_frame(&frame));
    ASSERT_EQ(frame.type, MC_LINK_READY);
    ASSERT_EQ(frame.len, 5u);

    ASSERT_TRUE(!read_main_link_frame(&frame));
    return 0;
}

int test_link_session(void)
{
    if (test_hello_emits_ready()) return 1;
    if (test_data_c2m_writes_rx_ring_and_sequence()) return 1;
    if (test_consumed_bytes_emit_credit()) return 1;
    if (test_wraps_server_tx_as_data_m2c()) return 1;
    if (test_reset_clears_rx_and_emits_ack_ready()) return 1;
    if (test_sequence_mismatch_preserves_error_frame()) return 1;
    if (test_read_tx_supports_partial_frame_reads()) return 1;
    if (test_queue_server_tx_failure_preserves_server_tx()) return 1;
    if (test_control_frames_precede_backed_up_data()) return 1;
    if (test_malformed_zero_length_hello_reports_bad_length()) return 1;
    if (test_null_public_api_guards()) return 1;
    if (test_control_waits_for_active_data_frame_boundary()) return 1;
    if (test_drop_queued_data_without_active_data_discards_data_m2c()) return 1;
    if (test_drop_queued_data_preserves_active_data_frame_tail()) return 1;
    return 0;
}
