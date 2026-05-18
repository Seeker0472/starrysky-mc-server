#include <stdint.h>
#include "platform_uart0.h"

#define ASSERT_TRUE(expr) do { if (!(expr)) return 1; } while (0)
#define ASSERT_EQ(a, b) do { if ((a) != (b)) return 1; } while (0)

int test_platform_uart0(void)
{
    uint8_t byte = 0u;

    ASSERT_TRUE(platform_uart0_decode_rx(0x0000004du, &byte));
    ASSERT_EQ(byte, 0x4du);

    ASSERT_TRUE(platform_uart0_decode_rx(0x000000ffu, &byte));
    ASSERT_EQ(byte, 0xffu);

    byte = 0xa5u;
    ASSERT_TRUE(!platform_uart0_decode_rx(0xffffffffu, &byte));
    ASSERT_EQ(byte, 0xa5u);

    ASSERT_TRUE(!platform_uart0_decode_rx(0xffffff6eu, &byte));
    ASSERT_EQ(byte, 0xa5u);

    ASSERT_TRUE(!platform_uart0_decode_rx(0x0000016eu, &byte));
    ASSERT_EQ(byte, 0xa5u);

    ASSERT_TRUE(!platform_uart0_decode_rx(0x00000040u, 0));
    return 0;
}
