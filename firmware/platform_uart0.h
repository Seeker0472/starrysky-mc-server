#ifndef PLATFORM_UART0_H
#define PLATFORM_UART0_H

#include <stdint.h>

int platform_uart0_decode_rx(uint32_t raw, uint8_t *byte);

#endif
