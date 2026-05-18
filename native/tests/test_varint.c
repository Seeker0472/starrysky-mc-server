#include <stdint.h>
#include <string.h>
#include "mc_varint.h"

#define ASSERT_TRUE(expr) do { if (!(expr)) return 1; } while (0)
#define ASSERT_EQ(a, b) do { if ((a) != (b)) return 1; } while (0)

int test_varint(void)
{
    uint8_t buf[8];
    int32_t value = 0;
    size_t used = 0;

    ASSERT_EQ(mc_varint_encode(0, buf, sizeof(buf)), 1u);
    ASSERT_EQ(buf[0], 0x00u);

    ASSERT_EQ(mc_varint_encode(300, buf, sizeof(buf)), 2u);
    ASSERT_EQ(buf[0], 0xACu);
    ASSERT_EQ(buf[1], 0x02u);

    ASSERT_TRUE(mc_varint_decode(buf, 2, &value, &used));
    ASSERT_EQ(value, 300);
    ASSERT_EQ(used, 2u);

    ASSERT_EQ(mc_varint_encode(-1, buf, sizeof(buf)), 5u);
    ASSERT_TRUE(mc_varint_decode(buf, 5, &value, &used));
    ASSERT_EQ(value, -1);
    ASSERT_EQ(used, 5u);

    buf[0] = 0x80;
    ASSERT_TRUE(!mc_varint_decode(buf, 1, &value, &used));

    buf[0] = 0x80;
    buf[1] = 0x80;
    buf[2] = 0x80;
    buf[3] = 0x80;
    buf[4] = 0x80;
    buf[5] = 0x00;
    ASSERT_TRUE(!mc_varint_decode(buf, 6, &value, &used));

    buf[0] = 0x80;
    buf[1] = 0x80;
    buf[2] = 0x80;
    buf[3] = 0x80;
    buf[4] = 0x10;
    ASSERT_TRUE(!mc_varint_decode(buf, 5, &value, &used));

    buf[0] = 0xFF;
    buf[1] = 0xFF;
    buf[2] = 0xFF;
    buf[3] = 0xFF;
    buf[4] = 0x0F;
    ASSERT_TRUE(mc_varint_decode(buf, 5, &value, &used));
    ASSERT_EQ(value, -1);
    ASSERT_EQ(used, 5u);

    return 0;
}
