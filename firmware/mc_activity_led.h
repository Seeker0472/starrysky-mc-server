#ifndef MC_ACTIVITY_LED_H
#define MC_ACTIVITY_LED_H

#include <stddef.h>
#include <stdint.h>
#include "mc_config.h"

#define MC_ACTIVITY_LED_SAMPLE_TICKS (MC_SERVER_TICKS_PER_SECOND / 10u)
#define MC_ACTIVITY_LED_LOW_MAX_BYTES 64u
#define MC_ACTIVITY_LED_MEDIUM_MAX_BYTES 512u
#define MC_ACTIVITY_LED_HALF_PERIOD_LOW (MC_SERVER_TICKS_PER_SECOND / 2u)
#define MC_ACTIVITY_LED_HALF_PERIOD_MEDIUM (MC_SERVER_TICKS_PER_SECOND / 6u)
#define MC_ACTIVITY_LED_HALF_PERIOD_HIGH (MC_SERVER_TICKS_PER_SECOND / 12u)

void mc_activity_led_init(uint32_t now_ticks);
void mc_activity_led_observe_bytes(size_t bytes);
void mc_activity_led_tick(uint32_t now_ticks);

#endif
