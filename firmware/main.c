#include "platform_ecos.h"
#include "mc_config.h"
#include "mc_firmware_config.h"
#include "mc_log.h"
#include "mc_ringbuf.h"
#include "mc_server.h"

static uint8_t rx_storage[MC_RX_RING_CAP];
static uint8_t tx_storage[MC_TX_RING_CAP];
static mc_ringbuf_t rx_ring;
static mc_ringbuf_t tx_ring;
static mc_server_t server;
static uint8_t tx_pending[MC_TICK_BUDGET_TX_BYTES];
static size_t tx_pending_len;
static size_t tx_pending_pos;
static volatile uint32_t rx_overflow_count;
static volatile uint32_t rx_parse_error_count;
static volatile uint32_t tx_backpressure_count;

static void server_trace(void *user, const mc_trace_event_t *event);

static int should_log_count(uint32_t count)
{
    return (count == 1u) || ((count != 0u) && ((count & (count - 1u)) == 0u));
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
    case MC_TRACE_BRIDGE_RESET:
        MC_LOGI("bridge reset magic");
        break;
    case MC_TRACE_BOOTSTRAP_STAGE:
        MC_LOGD("bootstrap stage=%d chunk=%d", (int)event->value0, (int)event->value1);
        break;
    case MC_TRACE_BOOTSTRAP_DONE:
        MC_LOGI("world bootstrap complete chunks=%d", (int)event->value1);
        break;
    case MC_TRACE_QUEUE_FULL:
        MC_LOGD("queue full stage=%d chunk=%d", (int)event->value0, (int)event->value1);
        break;
    default:
        break;
    }
}

static void reset_connection(const char *reason)
{
    mc_ringbuf_init(&rx_ring, rx_storage, sizeof(rx_storage));
    mc_ringbuf_init(&tx_ring, tx_storage, sizeof(tx_storage));
    mc_server_init(&server);
    mc_server_set_trace(&server, server_trace, 0);
    tx_pending_len = 0;
    tx_pending_pos = 0;
    MC_LOGI("connection reset reason=%s", reason);
}

static void pump_rx(void)
{
    uint8_t buf[MC_TICK_BUDGET_RX_BYTES];
    size_t n = platform_bridge_read(buf, sizeof(buf));
    if (n > 0u) {
        MC_LOGD("bridge rx bytes=%u", (unsigned int)n);
        MC_LOGT_HEXDUMP("bridge rx data", buf, n);
        size_t written = mc_ringbuf_write(&rx_ring, buf, n);
        if (written < n) {
            rx_overflow_count++;
            MC_LOGI("rx overflow count=%u written=%u wanted=%u",
                    (unsigned int)rx_overflow_count,
                    (unsigned int)written,
                    (unsigned int)n);
            reset_connection("rx_overflow");
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
            reset_connection("parse_error");
            return;
        }
        if (mc_server_take_tx_reset(&server)) {
            MC_LOGD("tx pending clear len=%u pos=%u",
                    (unsigned int)tx_pending_len,
                    (unsigned int)tx_pending_pos);
            tx_pending_len = 0;
            tx_pending_pos = 0;
        }
    }
    if (!mc_server_tick(&server, &tx_ring)) {
        tx_backpressure_count++;
        if (should_log_count(tx_backpressure_count)) {
            MC_LOGI("tx backpressure count=%u", (unsigned int)tx_backpressure_count);
        }
    }
}

static void pump_tx(void)
{
    if (tx_pending_pos < tx_pending_len) {
        size_t written = platform_bridge_write(tx_pending + tx_pending_pos,
                                               tx_pending_len - tx_pending_pos);
        if (written > 0u) {
            MC_LOGD("bridge tx bytes=%u", (unsigned int)written);
        }
        tx_pending_pos += written;
        if (tx_pending_pos < tx_pending_len) {
            return;
        }
        tx_pending_len = 0;
        tx_pending_pos = 0;
    }

    tx_pending_len = mc_ringbuf_read(&tx_ring, tx_pending, sizeof(tx_pending));
    if (tx_pending_len > 0u) {
        size_t written = platform_bridge_write(tx_pending, tx_pending_len);
        if (written > 0u) {
            MC_LOGD("bridge tx bytes=%u", (unsigned int)written);
        }
        tx_pending_pos = written;
        if (tx_pending_pos >= tx_pending_len) {
            tx_pending_len = 0;
            tx_pending_pos = 0;
        }
    }
}

void main(void)
{
    platform_init();
    MC_LOGI("mc-uart boot");
    MC_LOGI("uart0_baud=%u uart1_baud=%u bridge_uart=%u log_uart=%u",
            (unsigned int)MC_UART0_BAUD,
            (unsigned int)MC_UART1_BAUD,
            (unsigned int)MC_BRIDGE_UART_ID,
            (unsigned int)MC_LOG_UART_ID);
    reset_connection("boot");

    for (;;) {
        pump_rx();
        pump_server();
        pump_tx();
    }
}
