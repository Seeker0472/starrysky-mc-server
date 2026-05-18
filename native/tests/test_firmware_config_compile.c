#include "mc_firmware_config.h"

#ifndef MC_EXPECT_BRIDGE_UART_ID
#define MC_EXPECT_BRIDGE_UART_ID MC_UART_ID_0
#endif

#ifndef MC_EXPECT_LOG_UART_ID
#define MC_EXPECT_LOG_UART_ID MC_UART_ID_1
#endif

#if MC_BRIDGE_UART_ID != MC_EXPECT_BRIDGE_UART_ID
#error "unexpected bridge uart mapping"
#endif

#if MC_LOG_UART_ID != MC_EXPECT_LOG_UART_ID
#error "unexpected log uart mapping"
#endif

int firmware_config_compile_probe = 0;
