#include "mc_firmware_config.h"

#define ASSERT_EQ(a, b) do { if ((a) != (b)) return 1; } while (0)

int test_firmware_config(void)
{
    ASSERT_EQ(MC_UART_ID_0, 0u);
    ASSERT_EQ(MC_UART_ID_1, 1u);
    ASSERT_EQ(MC_UART0_BAUD, 115200u);
    ASSERT_EQ(MC_UART1_BAUD, 115200u);
    ASSERT_EQ(MC_BRIDGE_UART_ID, MC_UART_ID_0);
    ASSERT_EQ(MC_LOG_UART_ID, MC_UART_ID_1);
    ASSERT_EQ(MC_BRIDGE_UART_BAUD, 115200u);
    ASSERT_EQ(MC_LOG_UART_BAUD, 115200u);
    ASSERT_EQ(MC_LOG_LEVEL, MC_LOG_INFO);
    return 0;
}
