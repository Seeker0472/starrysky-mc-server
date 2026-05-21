#include "platform_psram.h"
#include "mc_firmware_config.h"
#include "platform_gpio0.h"

#if defined(__riscv)
#include "board.h"
#endif

#define PLATFORM_PSRAM_BASE_ADDR 0x40000000u
#define PLATFORM_PSRAM_TOTAL_BYTES (8u * 1024u * 1024u)
#define PLATFORM_PSRAM_TEST_SEED 0x12345678u
#define PLATFORM_PSRAM_QSPI_TIMEOUT_LOOPS 1000000u

void *platform_psram_base(void)
{
    return (void *)(uintptr_t)PLATFORM_PSRAM_BASE_ADDR;
}

size_t platform_psram_size(void)
{
    return PLATFORM_PSRAM_TOTAL_BYTES;
}

uint32_t platform_psram_pattern_word(size_t offset, uint32_t seed)
{
    uint32_t word_index = (uint32_t)(offset / sizeof(uint32_t));
    return (seed ^ 0xa5a50000u) ^ (word_index * 0x0000a5b9u);
}

void platform_psram_fill_pattern(volatile void *base, size_t bytes, uint32_t seed)
{
    volatile uint32_t *words = (volatile uint32_t *)base;
    size_t count = bytes / sizeof(uint32_t);

    for (size_t i = 0; i < count; i++) {
        words[i] = platform_psram_pattern_word(i * sizeof(uint32_t), seed);
    }
}

int platform_psram_check_pattern(volatile void *base,
                                 size_t bytes,
                                 uint32_t seed,
                                 volatile uint32_t **bad_word)
{
    volatile uint32_t *words = (volatile uint32_t *)base;
    size_t count;

    if (bad_word != 0) {
        *bad_word = 0;
    }
    if (base == 0 || bytes == 0u || (bytes % sizeof(uint32_t)) != 0u) {
        return 0;
    }

    count = bytes / sizeof(uint32_t);
    for (size_t i = 0; i < count; i++) {
        uint32_t expected = platform_psram_pattern_word(i * sizeof(uint32_t), seed);
        if (words[i] != expected) {
            if (bad_word != 0) {
                *bad_word = &words[i];
            }
            return 0;
        }
    }
    return 1;
}

#if defined(__riscv) && (MC_PSRAM_ENABLE || MC_PSRAM_FLOW_TEST)
static int platform_psram_wait_qspi_ready(void)
{
    for (uint32_t i = 0; i < PLATFORM_PSRAM_QSPI_TIMEOUT_LOOPS; i++) {
        if (REG_QSPI_0_STATUS == 1u) {
            return 1;
        }
    }
    return 0;
}

static int platform_psram_qspi_write8(uint8_t command)
{
    REG_QSPI_0_LEN = 0x80000u;
    REG_QSPI_0_TXFIFO = ((uint32_t)command) << 24;
    REG_QSPI_0_STATUS = 0x102u;
    return platform_psram_wait_qspi_ready();
}
#endif

int platform_psram_init(void)
{
#if defined(__riscv) && (MC_PSRAM_ENABLE || MC_PSRAM_FLOW_TEST)
    REG_QSPI_0_STATUS = 16u;
    REG_QSPI_0_STATUS = 0u;
    REG_QSPI_0_INTCFG = 0u;
    REG_QSPI_0_DUM = 0u;
    REG_QSPI_0_CLKDIV = 1u;

    platform_gpio0_reset_shadow(0u, 0u);

    REG_PSRAM_0_WC = 18u;
    REG_PSRAM_0_CHD = 4u;

    if (!platform_psram_qspi_write8(0x66u) ||
        !platform_psram_qspi_write8(0x99u) ||
        !platform_psram_qspi_write8(0x35u)) {
        return 0;
    }

    platform_gpio0_set_level_mask(PLATFORM_GPIO0_BIT_15, PLATFORM_GPIO0_BIT_15);
    REG_PSRAM_0_WC = 8u;
    REG_PSRAM_0_CHD = 0u;
    return 1;
#else
    return 1;
#endif
}

static platform_psram_test_result_t platform_psram_make_result(int ok,
                                                               platform_psram_stage_t stage,
                                                               volatile uint32_t *bad_word)
{
    platform_psram_test_result_t result;
    uintptr_t base_addr = PLATFORM_PSRAM_BASE_ADDR;
    uintptr_t end_addr = base_addr + PLATFORM_PSRAM_TOTAL_BYTES;

    result.ok = ok;
    result.stage = stage;
    result.bytes = MC_PSRAM_FLOW_TEST_BYTES;
    result.offset = 0u;
    result.expected = 0u;
    result.actual = 0u;

    if (!ok && bad_word != 0) {
        uintptr_t bad_addr = (uintptr_t)bad_word;

        if (bad_addr >= base_addr && bad_addr < end_addr) {
            result.offset = (uint32_t)(bad_addr - base_addr);
            result.expected = platform_psram_pattern_word(result.offset, PLATFORM_PSRAM_TEST_SEED);
            result.actual = *bad_word;
        }
    }

    return result;
}

static int platform_psram_flow_test_bytes_valid(void)
{
    return MC_PSRAM_FLOW_TEST_BYTES != 0u &&
           MC_PSRAM_FLOW_TEST_BYTES <= PLATFORM_PSRAM_TOTAL_BYTES &&
           (MC_PSRAM_FLOW_TEST_BYTES % sizeof(uint32_t)) == 0u;
}

platform_psram_test_result_t platform_psram_flow_test_prepare(void)
{
    volatile void *base = (volatile void *)(uintptr_t)PLATFORM_PSRAM_BASE_ADDR;
    volatile uint32_t *bad_word = 0;

    if (!platform_psram_flow_test_bytes_valid()) {
        return platform_psram_make_result(0, PLATFORM_PSRAM_STAGE_FILL, 0);
    }
    if (!platform_psram_init()) {
        return platform_psram_make_result(0, PLATFORM_PSRAM_STAGE_INIT, 0);
    }

    platform_psram_fill_pattern(base, MC_PSRAM_FLOW_TEST_BYTES, PLATFORM_PSRAM_TEST_SEED);
    if (!platform_psram_check_pattern(base, MC_PSRAM_FLOW_TEST_BYTES, PLATFORM_PSRAM_TEST_SEED, &bad_word)) {
        return platform_psram_make_result(0, PLATFORM_PSRAM_STAGE_FILL, bad_word);
    }
    return platform_psram_make_result(1, PLATFORM_PSRAM_STAGE_FILL, 0);
}

platform_psram_test_result_t platform_psram_flow_test_verify_after_reset(void)
{
    volatile void *base = (volatile void *)(uintptr_t)PLATFORM_PSRAM_BASE_ADDR;
    volatile uint32_t *bad_word = 0;

    if (!platform_psram_flow_test_bytes_valid()) {
        return platform_psram_make_result(0, PLATFORM_PSRAM_STAGE_VERIFY, 0);
    }

    if (!platform_psram_check_pattern(base, MC_PSRAM_FLOW_TEST_BYTES, PLATFORM_PSRAM_TEST_SEED, &bad_word)) {
        return platform_psram_make_result(0, PLATFORM_PSRAM_STAGE_VERIFY, bad_word);
    }
    return platform_psram_make_result(1, PLATFORM_PSRAM_STAGE_VERIFY, 0);
}
