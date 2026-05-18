#include "test_log_capture.h"
#include <string.h>

static char test_log_buffer[1024];
static size_t test_log_used;

void platform_log_write(const char *src, size_t len)
{
    size_t available = sizeof(test_log_buffer) - test_log_used - 1u;
    size_t copy_len = len;
    if (copy_len > available) {
        copy_len = available;
    }
    if (copy_len > 0u) {
        memcpy(test_log_buffer + test_log_used, src, copy_len);
        test_log_used += copy_len;
        test_log_buffer[test_log_used] = '\0';
    }
}

void test_log_reset(void)
{
    test_log_used = 0u;
    test_log_buffer[0] = '\0';
}

const char *test_log_data(void)
{
    return test_log_buffer;
}

size_t test_log_len(void)
{
    return test_log_used;
}

int test_log_contains(const char *needle)
{
    return strstr(test_log_buffer, needle) != 0;
}
