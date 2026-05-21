#include "platform_compat.h"
#include <stdint.h>

void *memmove(void *dest, const void *src, size_t n)
{
    unsigned char *d = (unsigned char *)dest;
    const unsigned char *s = (const unsigned char *)src;

    if (d == s || n == 0u) {
        return dest;
    }

    if (d < s) {
        while (n-- > 0u) {
            *d++ = *s++;
        }
    } else {
        d += n;
        s += n;
        while (n-- > 0u) {
            *--d = *--s;
        }
    }

    return dest;
}

static uint64_t udivmod64(uint64_t dividend, uint64_t divisor, uint64_t *remainder)
{
    uint64_t quotient = 0u;
    uint64_t rem = 0u;
    unsigned bit;

    if (divisor == 0u) {
        if (remainder != 0) {
            *remainder = dividend;
        }
        return UINT64_MAX;
    }

    for (bit = 64u; bit > 0u; bit--) {
        rem = (rem << 1u) | ((dividend >> (bit - 1u)) & 1u);
        if (rem >= divisor) {
            rem -= divisor;
            quotient |= UINT64_C(1) << (bit - 1u);
        }
    }

    if (remainder != 0) {
        *remainder = rem;
    }
    return quotient;
}

uint64_t __udivdi3(uint64_t dividend, uint64_t divisor)
{
    return udivmod64(dividend, divisor, 0);
}

uint64_t __umoddi3(uint64_t dividend, uint64_t divisor)
{
    uint64_t remainder = 0u;

    (void)udivmod64(dividend, divisor, &remainder);
    return remainder;
}
