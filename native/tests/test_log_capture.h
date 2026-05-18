#ifndef TEST_LOG_CAPTURE_H
#define TEST_LOG_CAPTURE_H

#include <stddef.h>

void test_log_reset(void);
const char *test_log_data(void);
size_t test_log_len(void);
int test_log_contains(const char *needle);

#endif
