#include "platform_ecos.h"
#include "mc_config.h"
#include "mc_firmware_config.h"
#include "mc_link_session.h"
#include "mc_log.h"
#include "mc_ringbuf.h"
#include "mc_server.h"

static uint8_t rx_storage[MC_RX_RING_CAP];
static uint8_t tx_storage[MC_TX_RING_CAP];
static mc_ringbuf_t rx_ring;
static mc_ringbuf_t tx_ring;
static mc_server_t server;
static mc_link_session_t link_session;
static uint8_t link_pending[MC_TICK_BUDGET_TX_BYTES];
static size_t link_pending_len;
static size_t link_pending_pos;
static volatile uint32_t rx_parse_error_count;
static volatile uint32_t tx_backpressure_count;
static volatile uint32_t link_rx_error_count;
static volatile uint32_t link_tx_backpressure_count;
static volatile int32_t tx_backpressure_stage;
static volatile int32_t tx_backpressure_chunk;

static void server_trace(void *user, const mc_trace_event_t *event);

static int should_log_count(uint32_t count)
{
    return (count == 1u) || ((count != 0u) && ((count & (count - 1u)) == 0u));
}

static int link_pending_is_data_m2c(void)
{
    mc_link_frame_t frame;
    if (link_pending_len == 0u || link_pending_pos > 0u ||
        link_pending_len > sizeof(link_pending)) {
        return 0;
    }
    return mc_link_decode_frame(link_pending, link_pending_len, &frame) &&
           frame.type == MC_LINK_DATA_M2C;
}

static void drop_stale_link_tx_data(void)
{
    mc_link_session_drop_queued_data(&link_session);
    /*
     * Logical Minecraft resets should drop stale DATA_M2C that has not
     * started on UART, but max frames can be split between link_pending and
     * the session's active DATA tail. Keep both chunks until that frame
     * finishes so the bridge parser stays synchronized. Control frames are
     * kept.
     */
    if (link_pending_pos == 0u &&
        link_session.active_tx_queue != MC_LINK_SESSION_TX_DATA &&
        link_pending_is_data_m2c()) {
        link_pending_len = 0;
        link_pending_pos = 0;
    }
}

static void server_trace(void *user, const mc_trace_event_t *event)
{
    (void)user;
    switch (event->type) {
    case MC_TRACE_FRAME_READY:
        MC_LOGD("frame ready len=%u state=%d", (unsigned int)event->value0, (int)event->value1);
        break;
    case MC_TRACE_HANDSHAKE:
        MC_LOGD("handshake next_state=%d proto=%d", (int)event->value0, (int)event->value1);
        break;
    case MC_TRACE_STATUS_REQUEST:
        MC_LOGD("status request");
        break;
    case MC_TRACE_STATUS_PING:
        MC_LOGD("status ping");
        break;
    case MC_TRACE_LOGIN_START:
        MC_LOGI("login username=%s", event->text ? event->text : "");
        break;
    case MC_TRACE_PLAY_ENTER:
        MC_LOGI("play enter");
        break;
    case MC_TRACE_BOOTSTRAP_STAGE:
        MC_LOGD("bootstrap stage=%d chunk=%d", (int)event->value0, (int)event->value1);
        break;
    case MC_TRACE_BOOTSTRAP_DONE:
        MC_LOGI("world bootstrap complete chunks=%d", (int)event->value1);
        break;
    case MC_TRACE_QUEUE_FULL:
        tx_backpressure_stage = event->value0;
        tx_backpressure_chunk = event->value1;
        break;
    default:
        break;
    }
}

static void reset_connection_quiet(void)
{
    mc_ringbuf_init(&rx_ring, rx_storage, sizeof(rx_storage));
    mc_ringbuf_init(&tx_ring, tx_storage, sizeof(tx_storage));
    mc_server_init(&server);
    mc_server_set_trace(&server, server_trace, 0);
    drop_stale_link_tx_data();
}

static void reset_connection(const char *reason)
{
    reset_connection_quiet();
    MC_LOGI("connection reset reason=%s", reason);
}

static void pump_link_rx(void)
{
    uint8_t buf[MC_TICK_BUDGET_RX_BYTES];
    size_t n = platform_bridge_read(buf, sizeof(buf));
    if (n > 0u) {
#if MC_LOG_LINK_UART_IO
        MC_LOGD("link rx uart bytes=%u", (unsigned int)n);
#endif
#if MC_TRACE_LINK_UART_RX_DATA
        MC_LOGT_HEXDUMP("link rx uart data", buf, n);
#endif
        if (!mc_link_session_receive_bytes(&link_session, buf, n)) {
            link_rx_error_count++;
            if (should_log_count(link_rx_error_count)) {
                MC_LOGI("link rx error count=%u", (unsigned int)link_rx_error_count);
                mc_link_parser_init(&link_session.parser);
                reset_connection("link_rx_error");
            } else {
                mc_link_parser_init(&link_session.parser);
                reset_connection_quiet();
            }
        }
        if (mc_link_session_take_reset_requested(&link_session)) {
            reset_connection("link_reset");
        }
    }
}

static void pump_server(void)
{
    uint8_t buf[MC_TICK_BUDGET_RX_BYTES];
    size_t n = mc_ringbuf_read(&rx_ring, buf, sizeof(buf));
    if (n > 0u) {
        if (!mc_server_receive(&server, buf, n, &tx_ring)) {
            rx_parse_error_count++;
            MC_LOGI("parse error count=%u", (unsigned int)rx_parse_error_count);
            (void)mc_link_session_reset_after_error(&link_session,
                                                    MC_LINK_ERR_PROTOCOL_STATE,
                                                    0u);
            reset_connection("parse_error");
            return;
        }
        mc_link_session_credit_consumed(&link_session, n);
        if (mc_server_take_tx_reset(&server)) {
            MC_LOGD("link pending preserved len=%u pos=%u",
                    (unsigned int)link_pending_len,
                    (unsigned int)link_pending_pos);
            drop_stale_link_tx_data();
        }
    }
    if (!mc_server_tick(&server, &tx_ring)) {
        tx_backpressure_count++;
        if (should_log_count(tx_backpressure_count)) {
            MC_LOGI("tx backpressure count=%u stage=%d chunk=%d",
                    (unsigned int)tx_backpressure_count,
                    (int)tx_backpressure_stage,
                    (int)tx_backpressure_chunk);
        }
    }
}

static void pump_link_tx(void)
{
    size_t budget = MC_LINK_TX_MAX_BYTES_PER_LOOP;

    if (link_pending_pos < link_pending_len) {
        size_t to_write = link_pending_len - link_pending_pos;
        if (to_write > budget) {
            to_write = budget;
        }
        size_t written = platform_bridge_write(link_pending + link_pending_pos,
                                               to_write);
        if (written > 0u) {
#if MC_LOG_LINK_UART_IO
            MC_LOGD("link tx uart bytes=%u", (unsigned int)written);
#endif
        }
        link_pending_pos += written;
        budget -= written;
        if (link_pending_pos < link_pending_len) {
            return;
        }
        link_pending_len = 0;
        link_pending_pos = 0;
        if (budget == 0u) {
            return;
        }
    }

    if (!mc_link_session_queue_server_tx(&link_session, &tx_ring)) {
        link_tx_backpressure_count++;
        if (should_log_count(link_tx_backpressure_count)) {
            MC_LOGI("link tx backpressure count=%u", (unsigned int)link_tx_backpressure_count);
        }
    }

    link_pending_len = mc_link_session_read_tx(&link_session, link_pending, sizeof(link_pending));
    if (link_pending_len > 0u) {
        size_t to_write = link_pending_len;
        size_t written;
        if (to_write > budget) {
            to_write = budget;
        }
        written = platform_bridge_write(link_pending, to_write);
        if (written > 0u) {
#if MC_LOG_LINK_UART_IO
            MC_LOGD("link tx uart bytes=%u", (unsigned int)written);
#endif
        }
        link_pending_pos = written;
        if (link_pending_pos >= link_pending_len) {
            link_pending_len = 0;
            link_pending_pos = 0;
        }
    }
}

void main(void)
{
    platform_init();
    mc_link_session_init(&link_session, &rx_ring);
    MC_LOGI("mc-uart boot");
    MC_LOGI("uart0_baud=%u uart1_baud=%u bridge_uart=%u log_uart=%u",
            (unsigned int)MC_UART0_BAUD,
            (unsigned int)MC_UART1_BAUD,
            (unsigned int)MC_BRIDGE_UART_ID,
            (unsigned int)MC_LOG_UART_ID);
    reset_connection("boot");

    for (;;) {
        pump_link_rx();
        pump_server();
        pump_link_tx();
    }
}
