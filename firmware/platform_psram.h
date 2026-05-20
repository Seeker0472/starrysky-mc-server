#ifndef PLATFORM_PSRAM_H
#define PLATFORM_PSRAM_H

#include <stddef.h>
#include <stdint.h>

typedef enum {
    PLATFORM_PSRAM_STAGE_NONE = 0,
    PLATFORM_PSRAM_STAGE_INIT = 1,
    PLATFORM_PSRAM_STAGE_FILL = 2,
    PLATFORM_PSRAM_STAGE_VERIFY = 3,
} platform_psram_stage_t;

typedef struct {
    int ok;
    platform_psram_stage_t stage;
    uint32_t bytes;
    uint32_t offset;
    uint32_t expected;
    uint32_t actual;
} platform_psram_test_result_t;

uint32_t platform_psram_pattern_word(size_t offset, uint32_t seed);
void platform_psram_fill_pattern(volatile void *base, size_t bytes, uint32_t seed);
int platform_psram_check_pattern(volatile void *base,
                                 size_t bytes,
                                 uint32_t seed,
                                 volatile uint32_t **bad_word);

int platform_psram_init(void);
void *platform_psram_base(void);
size_t platform_psram_size(void);
platform_psram_test_result_t platform_psram_flow_test_prepare(void);
platform_psram_test_result_t platform_psram_flow_test_verify_after_reset(void);

#endif
