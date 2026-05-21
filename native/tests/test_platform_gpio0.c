#include "platform_gpio0.h"
#include <stdint.h>

#define ASSERT_EQ(a, b) do { if ((a) != (b)) return 1; } while (0)

int test_platform_gpio0(void)
{
    platform_gpio0_reset_shadow(0xffffffffu, 0x8000u);
    ASSERT_EQ(platform_gpio0_ddr_shadow(), 0xffffffffu);
    ASSERT_EQ(platform_gpio0_dr_shadow(), 0x8000u);
    ASSERT_EQ(platform_gpio0_data_write_count(), 0u);

    platform_gpio0_set_direction_output(0x00000001u);
    ASSERT_EQ(platform_gpio0_ddr_shadow(), 0xfffffffeu);
    ASSERT_EQ(platform_gpio0_data_write_count(), 0u);

    platform_gpio0_set_level_mask(0x00000001u, 0x00000000u);
    ASSERT_EQ(platform_gpio0_dr_shadow(), 0x8000u);
    ASSERT_EQ(platform_gpio0_data_write_count(), 1u);

    platform_gpio0_set_level_mask(0x00000001u, 0x00000001u);
    ASSERT_EQ(platform_gpio0_dr_shadow(), 0x8001u);
    ASSERT_EQ(platform_gpio0_data_write_count(), 2u);

    platform_gpio0_set_level_mask(0x00008000u, 0x00000000u);
    ASSERT_EQ(platform_gpio0_dr_shadow(), 0x0001u);
    ASSERT_EQ(platform_gpio0_data_write_count(), 3u);

    platform_gpio0_set_level_mask(0x00008000u, 0x00008000u);
    ASSERT_EQ(platform_gpio0_dr_shadow(), 0x8001u);
    ASSERT_EQ(platform_gpio0_data_write_count(), 4u);
    return 0;
}
