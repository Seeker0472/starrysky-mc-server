#include "mc_activity_led.h"
#include "platform_gpio0.h"
#include <stdint.h>

#define ASSERT_EQ(a, b) do { if ((a) != (b)) return 1; } while (0)

static void reset_led_test(void)
{
    platform_gpio0_reset_shadow(0xffffffffu, PLATFORM_GPIO0_BIT_15);
}

static int expect_led_off(void)
{
    ASSERT_EQ(platform_gpio0_dr_shadow() & PLATFORM_GPIO0_BIT_0,
              PLATFORM_GPIO0_BIT_0);
    return 0;
}

static int expect_led_on(void)
{
    ASSERT_EQ(platform_gpio0_dr_shadow() & PLATFORM_GPIO0_BIT_0, 0u);
    return 0;
}

static void tick_sample_with_bytes(uint32_t now_ticks, size_t bytes)
{
    mc_activity_led_observe_bytes(bytes);
    mc_activity_led_tick(now_ticks);
}

static int test_activity_led_initializes_off_active_low(void)
{
    reset_led_test();

    mc_activity_led_init(0u);

    ASSERT_EQ(platform_gpio0_ddr_shadow() & PLATFORM_GPIO0_BIT_0, 0u);
    return expect_led_off();
}

static int test_activity_led_low_traffic_blinks_at_1hz(void)
{
    reset_led_test();
    mc_activity_led_init(0u);

    tick_sample_with_bytes(MC_ACTIVITY_LED_SAMPLE_TICKS,
                           MC_ACTIVITY_LED_LOW_MAX_BYTES);
    ASSERT_EQ(expect_led_on(), 0);

    tick_sample_with_bytes(MC_ACTIVITY_LED_SAMPLE_TICKS * 2u,
                           MC_ACTIVITY_LED_LOW_MAX_BYTES);
    tick_sample_with_bytes(MC_ACTIVITY_LED_SAMPLE_TICKS * 3u,
                           MC_ACTIVITY_LED_LOW_MAX_BYTES);
    tick_sample_with_bytes(MC_ACTIVITY_LED_SAMPLE_TICKS * 4u,
                           MC_ACTIVITY_LED_LOW_MAX_BYTES);
    tick_sample_with_bytes(MC_ACTIVITY_LED_SAMPLE_TICKS * 5u,
                           MC_ACTIVITY_LED_LOW_MAX_BYTES);
    mc_activity_led_tick((MC_ACTIVITY_LED_SAMPLE_TICKS +
                          MC_ACTIVITY_LED_HALF_PERIOD_LOW) - 1u);
    ASSERT_EQ(expect_led_on(), 0);

    tick_sample_with_bytes(MC_ACTIVITY_LED_SAMPLE_TICKS +
                           MC_ACTIVITY_LED_HALF_PERIOD_LOW,
                           MC_ACTIVITY_LED_LOW_MAX_BYTES);
    ASSERT_EQ(expect_led_off(), 0);

    tick_sample_with_bytes(MC_ACTIVITY_LED_SAMPLE_TICKS * 7u,
                           MC_ACTIVITY_LED_LOW_MAX_BYTES);
    tick_sample_with_bytes(MC_ACTIVITY_LED_SAMPLE_TICKS * 8u,
                           MC_ACTIVITY_LED_LOW_MAX_BYTES);
    tick_sample_with_bytes(MC_ACTIVITY_LED_SAMPLE_TICKS * 9u,
                           MC_ACTIVITY_LED_LOW_MAX_BYTES);
    tick_sample_with_bytes(MC_ACTIVITY_LED_SAMPLE_TICKS * 10u,
                           MC_ACTIVITY_LED_LOW_MAX_BYTES);
    tick_sample_with_bytes(MC_ACTIVITY_LED_SAMPLE_TICKS +
                           (MC_ACTIVITY_LED_HALF_PERIOD_LOW * 2u),
                           MC_ACTIVITY_LED_LOW_MAX_BYTES);
    return expect_led_on();
}

static int test_activity_led_medium_traffic_blinks_at_3hz(void)
{
    reset_led_test();
    mc_activity_led_init(0u);

    tick_sample_with_bytes(MC_ACTIVITY_LED_SAMPLE_TICKS,
                           MC_ACTIVITY_LED_LOW_MAX_BYTES + 1u);
    ASSERT_EQ(expect_led_on(), 0);

    tick_sample_with_bytes(MC_ACTIVITY_LED_SAMPLE_TICKS * 2u,
                           MC_ACTIVITY_LED_LOW_MAX_BYTES + 1u);
    mc_activity_led_tick((MC_ACTIVITY_LED_SAMPLE_TICKS +
                          MC_ACTIVITY_LED_HALF_PERIOD_MEDIUM) - 1u);
    ASSERT_EQ(expect_led_on(), 0);

    mc_activity_led_tick(MC_ACTIVITY_LED_SAMPLE_TICKS +
                         MC_ACTIVITY_LED_HALF_PERIOD_MEDIUM);
    ASSERT_EQ(expect_led_off(), 0);

    tick_sample_with_bytes(MC_ACTIVITY_LED_SAMPLE_TICKS * 3u,
                           MC_ACTIVITY_LED_LOW_MAX_BYTES + 1u);
    tick_sample_with_bytes(MC_ACTIVITY_LED_SAMPLE_TICKS * 4u,
                           MC_ACTIVITY_LED_LOW_MAX_BYTES + 1u);
    mc_activity_led_tick(MC_ACTIVITY_LED_SAMPLE_TICKS +
                         (MC_ACTIVITY_LED_HALF_PERIOD_MEDIUM * 2u));
    return expect_led_on();
}

static int test_activity_led_high_traffic_blinks_at_6hz(void)
{
    reset_led_test();
    mc_activity_led_init(0u);

    tick_sample_with_bytes(MC_ACTIVITY_LED_SAMPLE_TICKS,
                           MC_ACTIVITY_LED_MEDIUM_MAX_BYTES + 1u);
    ASSERT_EQ(expect_led_on(), 0);

    mc_activity_led_tick((MC_ACTIVITY_LED_SAMPLE_TICKS +
                          MC_ACTIVITY_LED_HALF_PERIOD_HIGH) - 1u);
    ASSERT_EQ(expect_led_on(), 0);

    mc_activity_led_tick(MC_ACTIVITY_LED_SAMPLE_TICKS +
                         MC_ACTIVITY_LED_HALF_PERIOD_HIGH);
    ASSERT_EQ(expect_led_off(), 0);

    tick_sample_with_bytes(MC_ACTIVITY_LED_SAMPLE_TICKS * 2u,
                           MC_ACTIVITY_LED_MEDIUM_MAX_BYTES + 1u);
    mc_activity_led_tick(MC_ACTIVITY_LED_SAMPLE_TICKS +
                         (MC_ACTIVITY_LED_HALF_PERIOD_HIGH * 2u));
    return expect_led_on();
}

static int test_activity_led_idle_window_forces_off(void)
{
    reset_led_test();
    mc_activity_led_init(0u);

    mc_activity_led_observe_bytes(MC_ACTIVITY_LED_MEDIUM_MAX_BYTES + 1u);
    mc_activity_led_tick(MC_ACTIVITY_LED_SAMPLE_TICKS);
    ASSERT_EQ(expect_led_on(), 0);

    mc_activity_led_tick(MC_ACTIVITY_LED_SAMPLE_TICKS * 2u);
    return expect_led_off();
}

static int test_activity_led_combines_observations(void)
{
    reset_led_test();
    mc_activity_led_init(0u);

    mc_activity_led_observe_bytes(MC_ACTIVITY_LED_LOW_MAX_BYTES);
    mc_activity_led_observe_bytes(1u);
    mc_activity_led_tick(MC_ACTIVITY_LED_SAMPLE_TICKS);
    ASSERT_EQ(expect_led_on(), 0);

    tick_sample_with_bytes(MC_ACTIVITY_LED_SAMPLE_TICKS * 2u,
                           MC_ACTIVITY_LED_LOW_MAX_BYTES + 1u);
    mc_activity_led_tick(MC_ACTIVITY_LED_SAMPLE_TICKS +
                         MC_ACTIVITY_LED_HALF_PERIOD_MEDIUM - 1u);
    ASSERT_EQ(expect_led_on(), 0);

    mc_activity_led_tick(MC_ACTIVITY_LED_SAMPLE_TICKS +
                         MC_ACTIVITY_LED_HALF_PERIOD_MEDIUM);
    return expect_led_off();
}

static int test_activity_led_redundant_ticks_do_not_rewrite_gpio(void)
{
    uint32_t writes;

    reset_led_test();
    mc_activity_led_init(0u);

    mc_activity_led_tick(1u);
    writes = platform_gpio0_data_write_count();
    mc_activity_led_tick(2u);
    mc_activity_led_tick(MC_ACTIVITY_LED_SAMPLE_TICKS - 1u);
    ASSERT_EQ(platform_gpio0_data_write_count(), writes);

    mc_activity_led_observe_bytes(1u);
    mc_activity_led_tick(MC_ACTIVITY_LED_SAMPLE_TICKS);
    writes = platform_gpio0_data_write_count();
    mc_activity_led_tick(MC_ACTIVITY_LED_SAMPLE_TICKS + 1u);
    mc_activity_led_tick((MC_ACTIVITY_LED_SAMPLE_TICKS * 2u) - 1u);
    ASSERT_EQ(platform_gpio0_data_write_count(), writes);
    return 0;
}

int test_activity_led(void)
{
    ASSERT_EQ(test_activity_led_initializes_off_active_low(), 0);
    ASSERT_EQ(test_activity_led_low_traffic_blinks_at_1hz(), 0);
    ASSERT_EQ(test_activity_led_medium_traffic_blinks_at_3hz(), 0);
    ASSERT_EQ(test_activity_led_high_traffic_blinks_at_6hz(), 0);
    ASSERT_EQ(test_activity_led_idle_window_forces_off(), 0);
    ASSERT_EQ(test_activity_led_combines_observations(), 0);
    ASSERT_EQ(test_activity_led_redundant_ticks_do_not_rewrite_gpio(), 0);
    return 0;
}
