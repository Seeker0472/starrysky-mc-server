#include "platform_uart0.h"

int platform_uart0_decode_rx(uint32_t raw, uint8_t *byte)
{
    if (byte == 0 || (raw & ~0xffu) != 0u) {
        return 0;
    }
    *byte = (uint8_t)raw;
    return 1;
}
