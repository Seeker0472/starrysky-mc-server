#include "platform_activity_led.h"
#include "platform_gpio0.h"

void platform_activity_led_init(void)
{
    platform_gpio0_set_direction_output(PLATFORM_GPIO0_BIT_0);
    platform_gpio0_set_level_mask(PLATFORM_GPIO0_BIT_0, PLATFORM_GPIO0_BIT_0);
}

void platform_activity_led_set(int on)
{
    platform_gpio0_set_level_mask(PLATFORM_GPIO0_BIT_0,
                                  on ? 0u : PLATFORM_GPIO0_BIT_0);
}
