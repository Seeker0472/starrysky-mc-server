#include "platform_gpio0.h"

#if defined(__riscv)
#include "board.h"
#endif

static uint32_t gpio0_ddr_shadow;
static uint32_t gpio0_dr_shadow;
static uint32_t gpio0_data_writes;

static void platform_gpio0_write_direction(void)
{
#if defined(__riscv)
    REG_GPIO_0_DDR = gpio0_ddr_shadow;
#endif
}

static void platform_gpio0_write_data(void)
{
#if defined(__riscv)
    REG_GPIO_0_DR = gpio0_dr_shadow;
#endif
}

void platform_gpio0_reset_shadow(uint32_t ddr, uint32_t dr)
{
    gpio0_ddr_shadow = ddr;
    gpio0_dr_shadow = dr;
    gpio0_data_writes = 0u;
    platform_gpio0_write_direction();
    platform_gpio0_write_data();
}

void platform_gpio0_set_direction_output(uint32_t mask)
{
    gpio0_ddr_shadow &= ~mask;
    platform_gpio0_write_direction();
}

void platform_gpio0_set_level_mask(uint32_t mask, uint32_t value_bits)
{
    gpio0_dr_shadow = (gpio0_dr_shadow & ~mask) | (value_bits & mask);
    gpio0_data_writes++;
    platform_gpio0_write_data();
}

uint32_t platform_gpio0_ddr_shadow(void)
{
    return gpio0_ddr_shadow;
}

uint32_t platform_gpio0_dr_shadow(void)
{
    return gpio0_dr_shadow;
}

uint32_t platform_gpio0_data_write_count(void)
{
    return gpio0_data_writes;
}
