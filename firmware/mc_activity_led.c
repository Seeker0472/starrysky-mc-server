#include "mc_activity_led.h"

#include "mc_firmware_config.h"
#include "platform_activity_led.h"

#if MC_UART_ACTIVITY_LED_ENABLE

typedef enum {
    MC_ACTIVITY_LED_CLASS_IDLE = 0,
    MC_ACTIVITY_LED_CLASS_LOW,
    MC_ACTIVITY_LED_CLASS_MEDIUM,
    MC_ACTIVITY_LED_CLASS_HIGH
} mc_activity_led_class_t;

static uint32_t activity_led_sample_start_ticks;
static uint32_t activity_led_phase_start_ticks;
static size_t activity_led_window_bytes;
static mc_activity_led_class_t activity_led_class;
static int activity_led_on;

static mc_activity_led_class_t classify_bytes(size_t bytes)
{
    if (bytes == 0u) {
        return MC_ACTIVITY_LED_CLASS_IDLE;
    }
    if (bytes <= MC_ACTIVITY_LED_LOW_MAX_BYTES) {
        return MC_ACTIVITY_LED_CLASS_LOW;
    }
    if (bytes <= MC_ACTIVITY_LED_MEDIUM_MAX_BYTES) {
        return MC_ACTIVITY_LED_CLASS_MEDIUM;
    }
    return MC_ACTIVITY_LED_CLASS_HIGH;
}

static uint32_t class_half_period(mc_activity_led_class_t traffic_class)
{
    switch (traffic_class) {
    case MC_ACTIVITY_LED_CLASS_LOW:
        return MC_ACTIVITY_LED_HALF_PERIOD_LOW;
    case MC_ACTIVITY_LED_CLASS_MEDIUM:
        return MC_ACTIVITY_LED_HALF_PERIOD_MEDIUM;
    case MC_ACTIVITY_LED_CLASS_HIGH:
        return MC_ACTIVITY_LED_HALF_PERIOD_HIGH;
    case MC_ACTIVITY_LED_CLASS_IDLE:
    default:
        return 0u;
    }
}

static void set_led(int on)
{
    on = on ? 1 : 0;
    if (activity_led_on == on) {
        return;
    }
    activity_led_on = on;
    platform_activity_led_set(on);
}

static void apply_class(mc_activity_led_class_t next_class, uint32_t now_ticks)
{
    uint32_t half_period;
    uint32_t elapsed_ticks;

    if (next_class == MC_ACTIVITY_LED_CLASS_IDLE) {
        activity_led_class = next_class;
        set_led(0);
        return;
    }

    if (next_class != activity_led_class) {
        activity_led_class = next_class;
        activity_led_phase_start_ticks = now_ticks;
        set_led(1);
        return;
    }

    half_period = class_half_period(activity_led_class);
    elapsed_ticks = now_ticks - activity_led_phase_start_ticks;
    if (half_period > 0u && elapsed_ticks >= half_period) {
        uint32_t half_periods = elapsed_ticks / half_period;

        activity_led_phase_start_ticks += half_periods * half_period;
        if ((half_periods & 1u) != 0u) {
            set_led(!activity_led_on);
        }
    }
}

void mc_activity_led_init(uint32_t now_ticks)
{
    platform_activity_led_init();
    activity_led_sample_start_ticks = now_ticks;
    activity_led_phase_start_ticks = now_ticks;
    activity_led_window_bytes = 0u;
    activity_led_class = MC_ACTIVITY_LED_CLASS_IDLE;
    activity_led_on = 0;
}

void mc_activity_led_observe_bytes(size_t bytes)
{
    activity_led_window_bytes += bytes;
}

void mc_activity_led_tick(uint32_t now_ticks)
{
    if ((uint32_t)(now_ticks - activity_led_sample_start_ticks) >=
        MC_ACTIVITY_LED_SAMPLE_TICKS) {
        activity_led_sample_start_ticks = now_ticks;
        apply_class(classify_bytes(activity_led_window_bytes), now_ticks);
        activity_led_window_bytes = 0u;
        return;
    }

    apply_class(activity_led_class, now_ticks);
}

#else

void mc_activity_led_init(uint32_t now_ticks)
{
    (void)now_ticks;
}

void mc_activity_led_observe_bytes(size_t bytes)
{
    (void)bytes;
}

void mc_activity_led_tick(uint32_t now_ticks)
{
    (void)now_ticks;
}

#endif
