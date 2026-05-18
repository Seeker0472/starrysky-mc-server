#include "mc_log.h"
#include "test_log_capture.h"

#define ASSERT_TRUE(x) do { if (!(x)) return 1; } while (0)
#define ASSERT_EQ(a, b) do { if ((a) != (b)) return 1; } while (0)

static unsigned int bump_count;

static unsigned int bump(void) __attribute__((unused));

static unsigned int bump(void)
{
    bump_count++;
    return bump_count;
}

int test_mc_log_info(void)
{
    bump_count = 0u;
    test_log_reset();
    MC_LOGI("boot baud=%u", 115200u);
    MC_LOGD("debug bump=%u", bump());
    ASSERT_TRUE(test_log_contains("[I] boot baud=115200\r\n"));
    ASSERT_TRUE(!test_log_contains("debug bump="));
    ASSERT_EQ(bump_count, 0u);
    return 0;
}
