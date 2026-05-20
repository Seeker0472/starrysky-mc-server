#include "platform_psram.h"
#include "mc_firmware_config.h"
#include <stdint.h>

#define ASSERT_TRUE(expr) do { if (!(expr)) return 1; } while (0)
#define ASSERT_EQ(a, b) do { if ((a) != (b)) return 1; } while (0)

#define EXPECTED_PSRAM_BASE ((void *)(uintptr_t)0x40000000u)
#define EXPECTED_PSRAM_SIZE ((size_t)8u * 1024u * 1024u)

int test_platform_psram(void)
{
    uint32_t storage[128];
    volatile uint32_t *bad = 0;

    ASSERT_TRUE(platform_psram_init());
    ASSERT_TRUE(platform_psram_base() == EXPECTED_PSRAM_BASE);
    ASSERT_EQ(platform_psram_size(), EXPECTED_PSRAM_SIZE);

    platform_psram_fill_pattern(storage, sizeof(storage), 0x12345678u);
    ASSERT_TRUE(platform_psram_check_pattern(storage, sizeof(storage), 0x12345678u, &bad));
    ASSERT_TRUE(bad == 0);

    storage[17] ^= 0x00010000u;
    ASSERT_TRUE(!platform_psram_check_pattern(storage, sizeof(storage), 0x12345678u, &bad));
    ASSERT_TRUE(bad == &storage[17]);

    platform_psram_fill_pattern(storage, sizeof(storage) - 1u, 0x89abcdefu);
    ASSERT_TRUE(!platform_psram_check_pattern(storage, sizeof(storage) - 1u, 0x89abcdefu, &bad));

    ASSERT_EQ(platform_psram_pattern_word(0u, 0x12345678u), 0x12345678u ^ 0xa5a50000u);
    ASSERT_EQ(platform_psram_pattern_word(4u, 0x12345678u), 0xb791f3c1u);
    return 0;
}

#ifdef MC_TEST_PSRAM_OVERSIZE_FLOW_BYTES
int main(void)
{
    platform_psram_test_result_t result = platform_psram_flow_test_verify_after_reset();

    ASSERT_TRUE(!result.ok);
    ASSERT_EQ(result.stage, PLATFORM_PSRAM_STAGE_VERIFY);
    ASSERT_EQ(result.bytes, MC_PSRAM_FLOW_TEST_BYTES);
    ASSERT_EQ(result.offset, 0u);
    ASSERT_EQ(result.expected, 0u);
    ASSERT_EQ(result.actual, 0u);
    return 0;
}
#endif
