#define MC_LOG_LEVEL MC_LOG_DEBUG
#include <limits.h>
#include <string.h>
#include <stdint.h>
#include "mc_log.h"
#include "test_log_capture.h"

#define ASSERT_STREQ(a, b) do { if (strcmp((a), (b)) != 0) return 1; } while (0)
#define ASSERT_EQ(a, b) do { if ((a) != (b)) return 1; } while (0)

static unsigned int trace_bump_count;

static const uint8_t *trace_bump_data(void) __attribute__((unused));

static const uint8_t *trace_bump_data(void)
{
    static const uint8_t bytes[] = { 0x0f };
    trace_bump_count++;
    return bytes;
}

int test_mc_log_debug(void)
{
    test_log_reset();
    MC_LOGD("rx=%u hex=%x char=%c percent=%%", 7u, 0x2au, 'z');
    ASSERT_STREQ(test_log_data(), "[D] rx=7 hex=2a char=z percent=%\r\n");

    test_log_reset();
    MC_LOGD("negative=%d", -42);
    ASSERT_STREQ(test_log_data(), "[D] negative=-42\r\n");

    test_log_reset();
    MC_LOGD("min=%d", INT_MIN);
    ASSERT_STREQ(test_log_data(), "[D] min=-2147483648\r\n");

    test_log_reset();
    MC_LOGD("str=%s", "ready");
    ASSERT_STREQ(test_log_data(), "[D] str=ready\r\n");

    test_log_reset();
    MC_LOGD("null=%s", (const char *)0);
    ASSERT_STREQ(test_log_data(), "[D] null=(null)\r\n");

    test_log_reset();
    MC_LOGD("trail=%");
    ASSERT_STREQ(test_log_data(), "[D] trail=%\r\n");

    test_log_reset();
    MC_LOGD("unknown=%q");
    ASSERT_STREQ(test_log_data(), "[D] unknown=%q\r\n");

    test_log_reset();
    trace_bump_count = 0u;
    MC_LOGT_HEXDUMP("bridge rx data", trace_bump_data(), 1u);
    ASSERT_STREQ(test_log_data(), "");
    ASSERT_EQ(trace_bump_count, 0u);
    return 0;
}
