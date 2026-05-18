#include "platform_compat.h"

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
