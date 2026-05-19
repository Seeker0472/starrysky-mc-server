#include "platform_ecos.h"
#include "generated/autoconf.h"
#include "board.h"
#include "mc_firmware_config.h"
#include "platform_uart0.h"
#include "timer.h"

#define UART1_TX_BUSY 0x100u
#define UART1_RX_EMPTY 0x080u
#define PLATFORM_LOG_ZERO_PROGRESS_LIMIT 1024u

static void uart0_tx_pace(void)
{
#if MC_UART0_TX_PACE_LOOPS > 0
    volatile uint32_t i;
    for (i = 0; i < MC_UART0_TX_PACE_LOOPS; i++) {
        __asm__ volatile ("" ::: "memory");
    }
#endif
}

static void uart0_init(void)
{
    REG_UART_0_CLKDIV = (CONFIG_CPU_FREQ_MHZ * 1000000u / MC_UART0_BAUD);
}

static size_t uart0_read(uint8_t *dst, size_t max_len)
{
    size_t n = 0;
    while (n < max_len) {
        uint32_t raw = REG_UART_0_DATA;
        if (!platform_uart0_decode_rx(raw, &dst[n])) {
            break;
        }
        n++;
    }
    return n;
}

static size_t uart0_write(const uint8_t *src, size_t max_len)
{
    size_t limit = max_len;
    size_t n = 0;
    if (limit > MC_UART0_WRITE_BURST_BYTES) {
        limit = MC_UART0_WRITE_BURST_BYTES;
    }
    while (n < limit) {
        REG_UART_0_DATA = src[n++];
        uart0_tx_pace();
    }
    return n;
}

static void uart1_init(void)
{
    REG_UART_1_LCR = 0x00;
    REG_UART_1_DIV = (CONFIG_CPU_FREQ_MHZ * 1000000u / MC_UART1_BAUD) - 1u;
    REG_UART_1_FCR = 0x0F;
    REG_UART_1_FCR = 0x0C;
    REG_UART_1_LCR = 0x1F;
}

static size_t uart1_read(uint8_t *dst, size_t max_len)
{
    size_t n = 0;
    while (n < max_len && (REG_UART_1_LSR & UART1_RX_EMPTY) == 0u) {
        dst[n++] = (uint8_t)REG_UART_1_TRX;
    }
    return n;
}

static size_t uart1_write(const uint8_t *src, size_t max_len)
{
    size_t n = 0;
    while (n < max_len && (REG_UART_1_LSR & UART1_TX_BUSY) == 0u) {
        REG_UART_1_TRX = src[n++];
    }
    return n;
}

void platform_init(void)
{
    uart0_init();
    uart1_init();
    sys_tick_init();
}

size_t platform_bridge_read(uint8_t *dst, size_t max_len)
{
#if MC_BRIDGE_UART_ID == MC_UART_ID_0
    return uart0_read(dst, max_len);
#else
    return uart1_read(dst, max_len);
#endif
}

size_t platform_bridge_write(const uint8_t *src, size_t max_len)
{
#if MC_BRIDGE_UART_ID == MC_UART_ID_0
    return uart0_write(src, max_len);
#else
    return uart1_write(src, max_len);
#endif
}

void platform_log_write(const char *src, size_t len)
{
    const uint8_t *bytes = (const uint8_t *)src;
    size_t pos = 0;
    uint32_t zero_progress = 0;
    while (pos < len) {
        size_t written;
#if MC_LOG_UART_ID == MC_UART_ID_0
        written = uart0_write(bytes + pos, len - pos);
#else
        written = uart1_write(bytes + pos, len - pos);
#endif
        if (written == 0u) {
            zero_progress++;
            if (zero_progress >= PLATFORM_LOG_ZERO_PROGRESS_LIMIT) {
                break;
            }
            continue;
        }
        pos += written;
        zero_progress = 0;
    }
}

uint32_t platform_ticks(void)
{
    return get_sys_tick();
}

size_t platform_uart_read(uint8_t *dst, size_t max_len)
{
    return platform_bridge_read(dst, max_len);
}

size_t platform_uart_write(const uint8_t *src, size_t max_len)
{
    return platform_bridge_write(src, max_len);
}

void platform_log(const char *msg)
{
    const char *end = msg;
    while (*end) {
        end++;
    }
    platform_log_write(msg, (size_t)(end - msg));
}
