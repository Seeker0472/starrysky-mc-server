#define MC_LOG_LEVEL MC_LOG_TRACE
#include <string.h>
#include "mc_log.h"
#include "test_log_capture.h"

#define ASSERT_STREQ(a, b) do { if (strcmp((a), (b)) != 0) return 1; } while (0)

int test_mc_log_trace(void)
{
    const uint8_t bytes[] = {
        0x0f, 0x00, 0x2f, 0x09, 0x31, 0x32, 0x37, 0x2e,
        0x30, 0x2e, 0x30, 0x2e, 0x31, 0x63, 0xdd, 0x02,
        0x0b, 0x00, 0x09
    };

    test_log_reset();
    MC_LOGT("rx data len=%u: %x %x %x", 3u, 0x0fu, 0x00u, 0xffu);
    ASSERT_STREQ(test_log_data(), "[T] rx data len=3: f 0 ff\r\n");

    test_log_reset();
    MC_LOGT_HEXDUMP("bridge rx data", bytes, sizeof(bytes));
    ASSERT_STREQ(test_log_data(),
                 "[T] bridge rx data len=19\r\n"
                 "[T] bridge rx data+0: 0f 00 2f 09 31 32 37 2e 30 2e 30 2e 31 63 dd 02\r\n"
                 "[T] bridge rx data+16: 0b 00 09\r\n");
    return 0;
}
