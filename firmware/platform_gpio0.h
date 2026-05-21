#ifndef PLATFORM_GPIO0_H
#define PLATFORM_GPIO0_H

#include <stdint.h>

#define PLATFORM_GPIO0_BIT_0 0x00000001u
#define PLATFORM_GPIO0_BIT_15 0x00008000u

void platform_gpio0_reset_shadow(uint32_t ddr, uint32_t dr);
void platform_gpio0_set_direction_output(uint32_t mask);
void platform_gpio0_set_level_mask(uint32_t mask, uint32_t value_bits);
uint32_t platform_gpio0_ddr_shadow(void);
uint32_t platform_gpio0_dr_shadow(void);
uint32_t platform_gpio0_data_write_count(void);

#endif
