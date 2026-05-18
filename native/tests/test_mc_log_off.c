#define MC_LOG_LEVEL MC_LOG_OFF
#include "mc_log.h"
#include "test_log_capture.h"

#define ASSERT_EQ(a, b) do { if ((a) != (b)) return 1; } while (0)

static unsigned int bump_count;

static unsigned int bump(void) __attribute__((unused));

static unsigned int bump(void)
{
    bump_count++;
    return bump_count;
}

int test_mc_log_off(void)
{
    bump_count = 0u;
    test_log_reset();
    MC_LOGI("info bump=%u", bump());
    MC_LOGD("debug bump=%u", bump());
    ASSERT_EQ(test_log_len(), 0u);
    ASSERT_EQ(bump_count, 0u);
    return 0;
}
