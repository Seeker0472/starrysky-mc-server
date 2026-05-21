#ifndef PLATFORM_ECOS_H
#define PLATFORM_ECOS_H

#include <stddef.h>
#include <stdint.h>

#ifndef MC_PLATFORM_TICK_RESET_THRESHOLD
#define MC_PLATFORM_TICK_RESET_THRESHOLD (UINT32_MAX / 2u)
#endif

typedef struct {
    uint32_t epoch_ticks;
} mc_tick_extender_t;

static inline void mc_tick_extender_init(mc_tick_extender_t *extender)
{
    if (extender) {
        extender->epoch_ticks = 0u;
    }
}

static inline uint32_t mc_tick_extender_update(mc_tick_extender_t *extender,
                                               uint32_t raw_ticks,
                                               int *reset_timer)
{
    if (reset_timer) {
        *reset_timer = 0;
    }
    if (!extender) {
        return raw_ticks;
    }
    if (raw_ticks >= (uint32_t)MC_PLATFORM_TICK_RESET_THRESHOLD) {
        extender->epoch_ticks += raw_ticks;
        if (reset_timer) {
            *reset_timer = 1;
        }
        return extender->epoch_ticks;
    }
    return extender->epoch_ticks + raw_ticks;
}

void platform_init(void);
size_t platform_bridge_read(uint8_t *dst, size_t max_len);
size_t platform_bridge_write(const uint8_t *src, size_t max_len);
void platform_log_write(const char *src, size_t len);
uint32_t platform_ticks(void);

size_t platform_uart_read(uint8_t *dst, size_t max_len);
size_t platform_uart_write(const uint8_t *src, size_t max_len);
void platform_log(const char *msg);

#endif
