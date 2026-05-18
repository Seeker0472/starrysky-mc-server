#include <stdint.h>
#include <string.h>
#include "mc_ringbuf.h"

#define ASSERT_TRUE(expr) do { if (!(expr)) return 1; } while (0)
#define ASSERT_EQ(a, b) do { if ((a) != (b)) return 1; } while (0)

int test_ringbuf(void)
{
    uint8_t storage[4];
    mc_ringbuf_t rb;
    uint8_t byte = 0;
    uint8_t out[4] = {0};

    mc_ringbuf_init(&rb, storage, sizeof(storage));
    ASSERT_EQ(mc_ringbuf_len(&rb), 0u);
    ASSERT_EQ(mc_ringbuf_free(&rb), 4u);

    ASSERT_TRUE(mc_ringbuf_push(&rb, 0x11));
    ASSERT_TRUE(mc_ringbuf_push(&rb, 0x22));
    ASSERT_TRUE(mc_ringbuf_push(&rb, 0x33));
    ASSERT_TRUE(mc_ringbuf_push(&rb, 0x44));
    ASSERT_TRUE(!mc_ringbuf_push(&rb, 0x55));
    ASSERT_EQ(mc_ringbuf_len(&rb), 4u);

    ASSERT_TRUE(mc_ringbuf_pop(&rb, &byte));
    ASSERT_EQ(byte, 0x11u);
    ASSERT_TRUE(mc_ringbuf_pop(&rb, &byte));
    ASSERT_EQ(byte, 0x22u);

    ASSERT_TRUE(mc_ringbuf_push(&rb, 0x55));
    ASSERT_TRUE(mc_ringbuf_push(&rb, 0x66));
    ASSERT_EQ(mc_ringbuf_len(&rb), 4u);

    ASSERT_EQ(mc_ringbuf_read(&rb, out, sizeof(out)), 4u);
    ASSERT_TRUE(out[0] == 0x33 && out[1] == 0x44 && out[2] == 0x55 && out[3] == 0x66);
    ASSERT_EQ(mc_ringbuf_len(&rb), 0u);

    ASSERT_EQ(mc_ringbuf_write(&rb, (const uint8_t *)"abc", 3), 3u);
    ASSERT_TRUE(mc_ringbuf_peek(&rb, 1, &byte));
    ASSERT_EQ(byte, (uint8_t)'b');
    mc_ringbuf_drop(&rb, 2);
    ASSERT_TRUE(mc_ringbuf_pop(&rb, &byte));
    ASSERT_EQ(byte, (uint8_t)'c');
    ASSERT_TRUE(!mc_ringbuf_pop(&rb, &byte));

    return 0;
}
