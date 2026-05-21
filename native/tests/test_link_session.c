#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include "mc_link.h"
#include "mc_link_session.h"
#include "mc_ringbuf.h"
#include "platform_ecos.h"
#include "mc_world.h"

#undef MC_LINK_TX_MAX_BYTES_PER_LOOP
#define MC_LINK_TX_MAX_BYTES_PER_LOOP 64u
#undef MC_LINK_TX_PENDING_BYTES
#define MC_LINK_TX_PENDING_BYTES 1024u

#define ASSERT_TRUE(expr) do { if (!(expr)) return 1; } while (0)
#define ASSERT_EQ(a, b) do { if ((a) != (b)) return 1; } while (0)
#define ASSERT_MEMEQ(a, b, n) do { if (memcmp((a), (b), (n)) != 0) return 1; } while (0)

static size_t platform_bridge_write_limit = (size_t)-1;
static size_t platform_bridge_write_last_len;
static uint8_t platform_bridge_write_capture[MC_LINK_MAX_FRAME_LEN];
static size_t platform_bridge_write_capture_len;
static uint32_t platform_tick_now;

void platform_init(void) {}

size_t platform_bridge_read(uint8_t *dst, size_t max_len)
{
    (void)dst;
    (void)max_len;
    return 0u;
}

size_t platform_bridge_write(const uint8_t *src, size_t max_len)
{
    size_t written = max_len;
    (void)src;
    platform_bridge_write_last_len = max_len;
    if (written > platform_bridge_write_limit) {
        written = platform_bridge_write_limit;
    }
    if (src != 0 && platform_bridge_write_capture_len + written <= sizeof(platform_bridge_write_capture)) {
        memcpy(platform_bridge_write_capture + platform_bridge_write_capture_len, src, written);
        platform_bridge_write_capture_len += written;
    }
    return written;
}

uint32_t platform_ticks(void)
{
    return platform_tick_now;
}

#define main firmware_main_entry
#include "../../firmware/main.c"
#undef main

static int firmware_enter_play_and_bootstrap(uint32_t now_ticks)
{
    uint8_t out[MC_TX_RING_CAP];
    const uint8_t handshake_login[] = {
        0x0f, 0x00, 0x2f, 0x09, '1','2','7','.','0','.','0','.','1', 0x63, 0xdd, 0x02
    };
    const uint8_t login_start[] = {
        0x09, 0x00, 0x07, 'p','l','a','y','e','r','1'
    };

    reset_connection_quiet();
    ASSERT_TRUE(mc_server_receive(&server, handshake_login, sizeof(handshake_login), &tx_ring));
    ASSERT_TRUE(mc_server_receive(&server, login_start, sizeof(login_start), &tx_ring));
    (void)mc_ringbuf_read(&tx_ring, out, sizeof(out));

    for (size_t i = 0u; i < mc_world_spawn_chunk_count() + 8u && !server.play_bootstrap_sent; i++) {
        ASSERT_TRUE(mc_server_tick_at(&server, &tx_ring, now_ticks));
        (void)mc_ringbuf_read(&tx_ring, out, sizeof(out));
    }
    ASSERT_TRUE(server.play_bootstrap_sent);
    return 1;
}

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

static int feed_encoded(mc_link_session_t *session,
                        uint8_t type,
                        uint16_t seq,
                        uint16_t ack,
                        const uint8_t *payload,
                        size_t payload_len)
{
    uint8_t frame[MC_LINK_MAX_FRAME_LEN];
    size_t frame_len = 0;
    ASSERT_TRUE(mc_link_encode(type, seq, ack, payload, payload_len, frame, sizeof(frame), &frame_len));
    ASSERT_TRUE(mc_link_session_receive_bytes(session, frame, frame_len));
    return 1;
}

static int feed_encoded_result(mc_link_session_t *session,
                               uint8_t type,
                               uint16_t seq,
                               uint16_t ack,
                               const uint8_t *payload,
                               size_t payload_len)
{
    uint8_t frame[MC_LINK_MAX_FRAME_LEN];
    size_t frame_len = 0;
    ASSERT_TRUE(mc_link_encode(type, seq, ack, payload, payload_len, frame, sizeof(frame), &frame_len));
    return mc_link_session_receive_bytes(session, frame, frame_len);
}

static uint16_t payload_u16(const mc_link_frame_t *frame, size_t offset)
{
    return (uint16_t)(frame->payload[offset] | (frame->payload[offset + 1u] << 8));
}

static int assert_ready_payload(const mc_link_frame_t *frame,
                                uint16_t negotiated_payload,
                                uint16_t credit_cap,
                                uint16_t initial_credit,
                                uint16_t supported_rate_mask,
                                uint8_t initial_rate_profile,
                                uint16_t flags)
{
    if (frame->type != MC_LINK_READY) return 0;
    if (frame->len != 11u) return 0;
    if (payload_u16(frame, 0u) != negotiated_payload) return 0;
    if (payload_u16(frame, 2u) != credit_cap) return 0;
    if (payload_u16(frame, 4u) != initial_credit) return 0;
    if (payload_u16(frame, 6u) != supported_rate_mask) return 0;
    if (frame->payload[8] != initial_rate_profile) return 0;
    if (payload_u16(frame, 9u) != flags) return 0;
    return 1;
}

static int test_v2_hello_emits_ready_with_capabilities(void)
{
    mc_link_session_t session;
    mc_link_frame_t frame;
    uint8_t rx_storage[256];
    mc_ringbuf_t rx;
    uint8_t hello[] = {
        0xf1u, 0x01u,
        0x00u, 0x02u,
        0xffu, 0x03u
    };
    mc_ringbuf_init(&rx, rx_storage, sizeof(rx_storage));
    mc_link_session_init(&session, &rx);
    ASSERT_TRUE(feed_encoded(&session, MC_LINK_HELLO, 0u, 0u, hello, sizeof(hello)));
    ASSERT_TRUE(read_frame_from_tx(&session, &frame));
    ASSERT_TRUE(assert_ready_payload(&frame,
                                     MC_LINK_DEFAULT_PAYLOAD,
                                     MC_LINK_INITIAL_CREDIT,
                                     MC_LINK_INITIAL_CREDIT,
                                     0x03ffu,
                                     0u,
                                     0u));
    return 0;
}

static int test_v2_hello_rate_mask_includes_p0_for_active_profile(void)
{
    mc_link_session_t session;
    mc_link_frame_t frame;
    uint8_t rx_storage[256];
    mc_ringbuf_t rx;
    uint8_t hello[] = {
        0xf1u, 0x01u,
        0x00u, 0x02u,
        0x02u, 0x00u
    };
    mc_ringbuf_init(&rx, rx_storage, sizeof(rx_storage));
    mc_link_session_init(&session, &rx);
    ASSERT_TRUE(feed_encoded(&session, MC_LINK_HELLO, 0u, 0u, hello, sizeof(hello)));
    ASSERT_TRUE(read_frame_from_tx(&session, &frame));
    ASSERT_TRUE(assert_ready_payload(&frame,
                                     MC_LINK_DEFAULT_PAYLOAD,
                                     MC_LINK_INITIAL_CREDIT,
                                     MC_LINK_INITIAL_CREDIT,
                                     0x0003u,
                                     0u,
                                     0u));
    return 0;
}

static int test_v2_hello_rate_mask_falls_back_to_p0(void)
{
    mc_link_session_t session;
    mc_link_frame_t frame;
    uint8_t rx_storage[256];
    mc_ringbuf_t rx;
    uint8_t zero_mask_hello[] = {
        0xf1u, 0x01u,
        0x00u, 0x02u,
        0x00u, 0x00u
    };
    uint8_t unsupported_mask_hello[] = {
        0xf1u, 0x01u,
        0x00u, 0x02u,
        0x00u, 0x04u
    };
    mc_ringbuf_init(&rx, rx_storage, sizeof(rx_storage));
    mc_link_session_init(&session, &rx);
    ASSERT_TRUE(feed_encoded(&session, MC_LINK_HELLO, 0u, 0u, zero_mask_hello, sizeof(zero_mask_hello)));
    ASSERT_TRUE(read_frame_from_tx(&session, &frame));
    ASSERT_TRUE(assert_ready_payload(&frame,
                                     MC_LINK_DEFAULT_PAYLOAD,
                                     MC_LINK_INITIAL_CREDIT,
                                     MC_LINK_INITIAL_CREDIT,
                                     0x0001u,
                                     0u,
                                     0u));
    ASSERT_TRUE(feed_encoded(&session, MC_LINK_HELLO, 0u, 0u, unsupported_mask_hello, sizeof(unsupported_mask_hello)));
    ASSERT_TRUE(read_frame_from_tx(&session, &frame));
    ASSERT_TRUE(assert_ready_payload(&frame,
                                     MC_LINK_DEFAULT_PAYLOAD,
                                     MC_LINK_INITIAL_CREDIT,
                                     MC_LINK_INITIAL_CREDIT,
                                     0x0001u,
                                     0u,
                                     0u));
    return 0;
}

static int test_v2_reset_preserves_hello_capabilities(void)
{
    mc_link_session_t session;
    mc_link_frame_t frame;
    uint8_t rx_storage[256];
    mc_ringbuf_t rx;
    uint8_t hello[] = {
        0x40u, 0x00u,
        0x20u, 0x00u,
        0x02u, 0x00u
    };
    mc_ringbuf_init(&rx, rx_storage, sizeof(rx_storage));
    mc_link_session_init(&session, &rx);
    ASSERT_TRUE(feed_encoded(&session, MC_LINK_HELLO, 0u, 0u, hello, sizeof(hello)));
    ASSERT_TRUE(read_frame_from_tx(&session, &frame));
    ASSERT_TRUE(assert_ready_payload(&frame, 64u, 32u, 32u, 0x0003u, 0u, 0u));
    ASSERT_TRUE(feed_encoded(&session, MC_LINK_RESET, 0x1234u, 0u, 0, 0));
    ASSERT_TRUE(read_frame_from_tx(&session, &frame));
    ASSERT_EQ(frame.type, MC_LINK_RESET_ACK);
    ASSERT_EQ(frame.seq, 0x1234u);
    ASSERT_TRUE(read_frame_from_tx(&session, &frame));
    ASSERT_TRUE(assert_ready_payload(&frame, 64u, 32u, 32u, 0x0003u, 0u, 0u));
    return 0;
}

static int test_v2_reset_after_error_preserves_hello_capabilities(void)
{
    mc_link_session_t session;
    mc_link_frame_t frame;
    uint8_t rx_storage[256];
    mc_ringbuf_t rx;
    uint8_t hello[] = {
        0x30u, 0x00u,
        0x18u, 0x00u,
        0x04u, 0x00u
    };
    mc_ringbuf_init(&rx, rx_storage, sizeof(rx_storage));
    mc_link_session_init(&session, &rx);
    ASSERT_TRUE(feed_encoded(&session, MC_LINK_HELLO, 0u, 0u, hello, sizeof(hello)));
    ASSERT_TRUE(read_frame_from_tx(&session, &frame));
    ASSERT_TRUE(assert_ready_payload(&frame, 48u, 24u, 24u, 0x0005u, 0u, 0u));
    ASSERT_TRUE(mc_link_session_reset_after_error(&session, MC_LINK_ERR_PROTOCOL_STATE, 0x7au));
    ASSERT_TRUE(read_frame_from_tx(&session, &frame));
    ASSERT_EQ(frame.type, MC_LINK_ERROR);
    ASSERT_EQ(frame.len, 2u);
    ASSERT_EQ(frame.payload[0], MC_LINK_ERR_PROTOCOL_STATE);
    ASSERT_EQ(frame.payload[1], 0x7au);
    ASSERT_TRUE(read_frame_from_tx(&session, &frame));
    ASSERT_TRUE(assert_ready_payload(&frame, 48u, 24u, 24u, 0x0005u, 0u, 0u));
    return 0;
}

static int test_v2_data_c2m_writes_rx_and_emits_ack(void)
{
    mc_link_session_t session;
    mc_link_frame_t frame;
    uint8_t rx_storage[256];
    uint8_t out[8];
    mc_ringbuf_t rx;
    uint8_t payload[] = {'h', 'i'};
    mc_ringbuf_init(&rx, rx_storage, sizeof(rx_storage));
    mc_link_session_init(&session, &rx);
    ASSERT_TRUE(feed_encoded(&session, MC_LINK_DATA_C2M, 0u, 0u, payload, sizeof(payload)));
    ASSERT_EQ(mc_ringbuf_read(&rx, out, sizeof(out)), 2u);
    ASSERT_MEMEQ(out, payload, sizeof(payload));
    ASSERT_TRUE(read_frame_from_tx(&session, &frame));
    ASSERT_EQ(frame.type, MC_LINK_ACK_C2M);
    ASSERT_EQ(frame.ack, 1u);
    ASSERT_EQ(frame.len, 2u);
    ASSERT_EQ(payload_u16(&frame, 0u), 254u);
    return 0;
}

static int test_v2_rate_probe_ack_echoes_sleep_us_and_nonce(void)
{
    mc_link_session_t session;
    mc_link_frame_t frame;
    uint8_t rx_storage[256];
    mc_ringbuf_t rx;
    uint8_t probe[] = {0xdc, 0x05u, 0x34u, 0x12u};
    mc_ringbuf_init(&rx, rx_storage, sizeof(rx_storage));
    mc_link_session_init(&session, &rx);
    ASSERT_TRUE(feed_encoded(&session, MC_LINK_RATE_PROBE, 9u, 0u, probe, sizeof(probe)));
    ASSERT_TRUE(read_frame_from_tx(&session, &frame));
    ASSERT_EQ(frame.type, MC_LINK_RATE_PROBE_ACK);
    ASSERT_EQ(frame.seq, 9u);
    ASSERT_EQ(frame.len, sizeof(probe));
    ASSERT_MEMEQ(frame.payload, probe, sizeof(probe));
    return 0;
}

static int test_v2_consumed_after_data_emits_unsolicited_ack_zero(void)
{
    mc_link_session_t session;
    mc_link_frame_t frame;
    uint8_t rx_storage[256];
    uint8_t out[8];
    mc_ringbuf_t rx;
    uint8_t payload[] = {'o', 'k'};
    mc_ringbuf_init(&rx, rx_storage, sizeof(rx_storage));
    mc_link_session_init(&session, &rx);
    ASSERT_TRUE(feed_encoded(&session, MC_LINK_DATA_C2M, 0u, 0u, payload, sizeof(payload)));
    ASSERT_TRUE(read_frame_from_tx(&session, &frame));
    ASSERT_EQ(frame.type, MC_LINK_ACK_C2M);
    ASSERT_EQ(frame.ack, 1u);
    ASSERT_EQ(payload_u16(&frame, 0u), 254u);
    ASSERT_EQ(mc_ringbuf_read(&rx, out, sizeof(out)), 2u);
    ASSERT_MEMEQ(out, payload, sizeof(payload));
    mc_link_session_credit_consumed(&session, 2u);
    ASSERT_TRUE(read_frame_from_tx(&session, &frame));
    ASSERT_EQ(frame.type, MC_LINK_ACK_C2M);
    ASSERT_EQ(frame.ack, 0u);
    ASSERT_EQ(frame.len, 2u);
    ASSERT_EQ(payload_u16(&frame, 0u), 256u);
    return 0;
}

static int test_v2_consumed_bytes_emit_unsolicited_ack_credit(void)
{
    mc_link_session_t session;
    mc_link_frame_t frame;
    uint8_t rx_storage[256];
    mc_ringbuf_t rx;
    mc_ringbuf_init(&rx, rx_storage, sizeof(rx_storage));
    mc_link_session_init(&session, &rx);
    mc_link_session_credit_consumed(&session, 37u);
    ASSERT_TRUE(read_frame_from_tx(&session, &frame));
    ASSERT_EQ(frame.type, MC_LINK_ACK_C2M);
    ASSERT_EQ(frame.ack, 0u);
    ASSERT_EQ(frame.len, 2u);
    ASSERT_EQ((uint16_t)(frame.payload[0] | (frame.payload[1] << 8)), 256u);
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
    ASSERT_TRUE(feed_encoded(&session, MC_LINK_RESET, 0x4321u, 0u, 0, 0));
    ASSERT_EQ(mc_ringbuf_len(&rx), 0u);
    ASSERT_TRUE(read_frame_from_tx(&session, &frame));
    ASSERT_EQ(frame.type, MC_LINK_RESET_ACK);
    ASSERT_EQ(frame.seq, 0x4321u);
    ASSERT_TRUE(read_frame_from_tx(&session, &frame));
    ASSERT_EQ(frame.type, MC_LINK_READY);
    return 0;
}

static int test_duplicate_data_c2m_emits_ack_without_rewriting_rx(void)
{
    mc_link_session_t session;
    mc_link_frame_t frame;
    uint8_t rx_storage[256];
    uint8_t out[8];
    mc_ringbuf_t rx;
    uint8_t payload[] = {'a'};
    mc_ringbuf_init(&rx, rx_storage, sizeof(rx_storage));
    mc_link_session_init(&session, &rx);
    ASSERT_TRUE(feed_encoded(&session, MC_LINK_DATA_C2M, 0u, 0u, payload, sizeof(payload)));
    ASSERT_TRUE(read_frame_from_tx(&session, &frame));
    ASSERT_EQ(frame.type, MC_LINK_ACK_C2M);
    ASSERT_EQ(frame.ack, 1u);
    ASSERT_TRUE(feed_encoded(&session, MC_LINK_DATA_C2M, 0u, 0u, payload, sizeof(payload)));
    ASSERT_EQ(mc_ringbuf_read(&rx, out, sizeof(out)), 1u);
    ASSERT_MEMEQ(out, payload, sizeof(payload));
    ASSERT_EQ(mc_ringbuf_len(&rx), 0u);
    ASSERT_EQ(session.c2m_duplicate_frames, 1u);
    ASSERT_TRUE(read_frame_from_tx(&session, &frame));
    ASSERT_EQ(frame.type, MC_LINK_ACK_C2M);
    ASSERT_EQ(frame.ack, 1u);
    ASSERT_EQ(frame.len, 2u);
    ASSERT_EQ(payload_u16(&frame, 0u), 255u);
    return 0;
}

static int test_out_of_order_data_c2m_preserves_error_frame(void)
{
    mc_link_session_t session;
    mc_link_frame_t frame;
    uint8_t rx_storage[256];
    mc_ringbuf_t rx;
    uint8_t skipped[] = {'b'};
    mc_ringbuf_init(&rx, rx_storage, sizeof(rx_storage));
    mc_link_session_init(&session, &rx);
    ASSERT_TRUE(!feed_encoded_result(&session, MC_LINK_DATA_C2M, 2u, 0u, skipped, sizeof(skipped)));
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
    uint8_t tx_storage[MC_LINK_MAX_PAYLOAD];
    uint8_t payload[MC_LINK_MAX_PAYLOAD];
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
    session.negotiated_payload = MC_LINK_MAX_PAYLOAD;
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
    ASSERT_TRUE(total <= MC_LINK_MAX_FRAME_LEN);
    ASSERT_EQ(result, MC_LINK_PARSE_FRAME);
    ASSERT_EQ(frame.type, MC_LINK_DATA_M2C);
    ASSERT_EQ(frame.len, MC_LINK_MAX_PAYLOAD);
    ASSERT_MEMEQ(frame.payload, payload, sizeof(payload));
    return 0;
}

static int test_queue_server_tx_failure_preserves_server_tx(void)
{
    mc_link_session_t session;
    uint8_t rx_storage[256];
    uint8_t tx_storage[MC_LINK_MAX_PAYLOAD];
    uint8_t filler_payload[MC_LINK_MAX_PAYLOAD];
    uint8_t payload[] = {'k', 'e', 'e', 'p'};
    uint8_t out[MC_LINK_MAX_FRAME_LEN];
    mc_ringbuf_t rx;
    mc_ringbuf_t server_tx;
    mc_ringbuf_init(&rx, rx_storage, sizeof(rx_storage));
    mc_ringbuf_init(&server_tx, tx_storage, sizeof(tx_storage));
    mc_link_session_init(&session, &rx);
    session.negotiated_payload = MC_LINK_MAX_PAYLOAD;
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
    uint8_t tx_storage[MC_LINK_MAX_PAYLOAD];
    uint8_t filler_payload[MC_LINK_MAX_PAYLOAD];
    mc_ringbuf_t rx;
    mc_ringbuf_t server_tx;
    mc_ringbuf_init(&rx, rx_storage, sizeof(rx_storage));
    mc_ringbuf_init(&server_tx, tx_storage, sizeof(tx_storage));
    mc_link_session_init(&session, &rx);
    session.negotiated_payload = MC_LINK_MAX_PAYLOAD;
    memset(filler_payload, 0xa5, sizeof(filler_payload));
    for (size_t i = 0u; i < 4u; ++i) {
        ASSERT_EQ(mc_ringbuf_write(&server_tx, filler_payload, sizeof(filler_payload)), sizeof(filler_payload));
        ASSERT_TRUE(mc_link_session_queue_server_tx(&session, &server_tx));
        ASSERT_EQ(mc_ringbuf_len(&server_tx), 0u);
    }
    mc_link_session_credit_consumed(&session, 1u);
    ASSERT_TRUE(read_frame_from_tx(&session, &frame));
    ASSERT_EQ(frame.type, MC_LINK_ACK_C2M);
    ASSERT_EQ(frame.len, 2u);
    ASSERT_EQ((uint16_t)(frame.payload[0] | (frame.payload[1] << 8)), 256u);
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
    ASSERT_TRUE(feed_encoded(&session, MC_LINK_HELLO, 0u, 0u, 0, 0));
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
    uint8_t tx_storage[MC_LINK_MAX_PAYLOAD];
    uint8_t payload[MC_LINK_MAX_PAYLOAD];
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
    session.negotiated_payload = MC_LINK_MAX_PAYLOAD;
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
            ASSERT_EQ(frame.len, MC_LINK_MAX_PAYLOAD);
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
    ASSERT_EQ(frame.type, MC_LINK_ACK_C2M);
    ASSERT_EQ(frame.len, 2u);
    ASSERT_EQ((uint16_t)(frame.payload[0] | (frame.payload[1] << 8)), 256u);
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
    uint8_t tx_storage[MC_LINK_MAX_PAYLOAD + 16u];
    uint8_t first[MC_LINK_MAX_PAYLOAD];
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
    session.negotiated_payload = MC_LINK_MAX_PAYLOAD;
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
            ASSERT_EQ(frame.len, MC_LINK_MAX_PAYLOAD);
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

static int feed_main_data_c2m(uint16_t seq, const uint8_t *payload, size_t payload_len)
{
    uint8_t encoded[MC_LINK_MAX_FRAME_LEN];
    size_t encoded_len = 0u;
    ASSERT_TRUE(mc_link_encode(MC_LINK_DATA_C2M,
                               seq,
                               0u,
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

static void reset_main_tx_probe(void)
{
    platform_bridge_write_limit = (size_t)-1;
    platform_bridge_write_last_len = 0u;
    platform_bridge_write_capture_len = 0u;
    link_pending_len = 0u;
    link_pending_pos = 0u;
    mc_ringbuf_init(&rx_ring, rx_storage, sizeof(rx_storage));
    mc_ringbuf_init(&tx_ring, tx_storage, sizeof(tx_storage));
    mc_link_session_init(&link_session, &rx_ring);
}

static int test_firmware_main_pump_link_tx_obeys_loop_budget(void)
{
    uint8_t payload[MC_LINK_MAX_PAYLOAD];
    mc_link_frame_t frame;
    mc_link_parser_t parser;
    mc_link_parse_result_t result = MC_LINK_PARSE_NEED_MORE;

    for (size_t i = 0u; i < sizeof(payload); ++i) {
        payload[i] = (uint8_t)(i ^ 0xa5u);
    }

    reset_main_tx_probe();
    ASSERT_EQ(mc_ringbuf_write(&tx_ring, payload, sizeof(payload)), sizeof(payload));

    pump_link_tx();
    ASSERT_TRUE(platform_bridge_write_last_len <= MC_LINK_TX_MAX_BYTES_PER_LOOP);
    ASSERT_EQ(link_pending_pos, MC_LINK_TX_MAX_BYTES_PER_LOOP);
    ASSERT_EQ(sizeof(link_pending), MC_LINK_TX_PENDING_BYTES);
    ASSERT_TRUE(link_pending_len > link_pending_pos);

    platform_bridge_write_limit = 16u;
    pump_link_tx();
    ASSERT_TRUE(platform_bridge_write_last_len <= MC_LINK_TX_MAX_BYTES_PER_LOOP);
    ASSERT_EQ(link_pending_pos, MC_LINK_TX_MAX_BYTES_PER_LOOP + 16u);
    ASSERT_TRUE(link_pending_len > link_pending_pos);

    platform_bridge_write_limit = (size_t)-1;
    mc_link_parser_init(&parser);
    while (link_pending_pos < link_pending_len) {
        size_t previous_pos = link_pending_pos;
        pump_link_tx();
        ASSERT_TRUE(platform_bridge_write_last_len <= MC_LINK_TX_MAX_BYTES_PER_LOOP);
        ASSERT_TRUE(link_pending_pos > previous_pos || link_pending_len == 0u);
    }

    ASSERT_EQ(link_pending_len, 0u);
    ASSERT_EQ(link_pending_pos, 0u);
    ASSERT_TRUE(link_session.active_tx_queue == MC_LINK_SESSION_TX_NONE);
    ASSERT_EQ(mc_ringbuf_len(&tx_ring), 0u);

    result = mc_link_parser_feed(&parser,
                                 platform_bridge_write_capture,
                                 platform_bridge_write_capture_len,
                                 &frame);
    ASSERT_EQ(result, MC_LINK_PARSE_FRAME);
    ASSERT_EQ(frame.type, MC_LINK_DATA_M2C);
    ASSERT_EQ(frame.len, MC_LINK_MAX_PAYLOAD);
    ASSERT_MEMEQ(frame.payload, payload, sizeof(payload));
    return 0;
}

static int test_firmware_main_queues_keepalive_while_m2c_busy(void)
{
    uint8_t out[128];
    uint8_t busy = 0xaa;
    uint32_t now = 1u;

    reset_main_tx_probe();
    ASSERT_TRUE(firmware_enter_play_and_bootstrap(now));
    ASSERT_EQ(mc_ringbuf_write(&link_session.tx, &busy, 1u), 1u);
    server.last_keepalive_tick = now;
    platform_tick_now = now + MC_KEEPALIVE_INTERVAL_TICKS;

    pump_server();
    ASSERT_EQ(server.keepalive_pending, 1u);
    ASSERT_TRUE(mc_ringbuf_read(&tx_ring, out, sizeof(out)) > 0u);
    ASSERT_TRUE(server.last_keepalive_tick != 0u);
    return 0;
}

static int test_tick_extender_resets_before_hardware_timer_stalls(void)
{
    mc_tick_extender_t extender;
    uint32_t edge = (uint32_t)MC_PLATFORM_TICK_RESET_THRESHOLD;
    int reset_timer = -1;

    mc_tick_extender_init(&extender);

    ASSERT_EQ(mc_tick_extender_update(&extender, edge - 1u, &reset_timer), edge - 1u);
    ASSERT_EQ(reset_timer, 0);

    ASSERT_EQ(mc_tick_extender_update(&extender, edge, &reset_timer), edge);
    ASSERT_EQ(reset_timer, 1);

    ASSERT_EQ(mc_tick_extender_update(&extender, 250u, &reset_timer), edge + 250u);
    ASSERT_EQ(reset_timer, 0);

    return 0;
}

int test_firmware_main(void)
{
    mc_link_frame_t frame;
    const uint8_t invalid_varint_prefix[] = {0x80u, 0x80u, 0x80u, 0x80u, 0x80u, 0x00u};

    mc_link_session_init(&link_session, &rx_ring);
    platform_tick_now = 0u;
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
    ASSERT_EQ(frame.type, MC_LINK_ACK_C2M);
    ASSERT_EQ(frame.ack, 1u);
    ASSERT_EQ(frame.len, 2u);

    ASSERT_TRUE(read_main_link_frame(&frame));
    ASSERT_EQ(frame.type, MC_LINK_ERROR);
    ASSERT_EQ(frame.len, 2u);
    ASSERT_EQ(frame.payload[0], MC_LINK_ERR_PROTOCOL_STATE);

    ASSERT_TRUE(read_main_link_frame(&frame));
    ASSERT_EQ(frame.type, MC_LINK_READY);
    ASSERT_EQ(frame.len, 11u);

    ASSERT_TRUE(!read_main_link_frame(&frame));

    if (test_firmware_main_pump_link_tx_obeys_loop_budget()) return 1;
    if (test_firmware_main_queues_keepalive_while_m2c_busy()) return 1;
    if (test_tick_extender_resets_before_hardware_timer_stalls()) return 1;
    return 0;
}

int test_link_session(void)
{
    if (test_v2_hello_emits_ready_with_capabilities()) return 1;
    if (test_v2_hello_rate_mask_includes_p0_for_active_profile()) return 1;
    if (test_v2_hello_rate_mask_falls_back_to_p0()) return 1;
    if (test_v2_reset_preserves_hello_capabilities()) return 1;
    if (test_v2_reset_after_error_preserves_hello_capabilities()) return 1;
    if (test_v2_data_c2m_writes_rx_and_emits_ack()) return 1;
    if (test_v2_rate_probe_ack_echoes_sleep_us_and_nonce()) return 1;
    if (test_v2_consumed_after_data_emits_unsolicited_ack_zero()) return 1;
    if (test_v2_consumed_bytes_emit_unsolicited_ack_credit()) return 1;
    if (test_wraps_server_tx_as_data_m2c()) return 1;
    if (test_reset_clears_rx_and_emits_ack_ready()) return 1;
    if (test_duplicate_data_c2m_emits_ack_without_rewriting_rx()) return 1;
    if (test_out_of_order_data_c2m_preserves_error_frame()) return 1;
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
