#include "mc_log.h"
#include "platform_ecos.h"

static void log_write_char(char c)
{
    platform_log_write(&c, 1u);
}

static void log_write_str(const char *s)
{
    const char *p;
    if (s == 0) {
        s = "(null)";
    }
    p = s;
    while (*p) {
        p++;
    }
    platform_log_write(s, (size_t)(p - s));
}

static void log_write_unsigned(unsigned int value, unsigned int base)
{
    char digits[sizeof(unsigned int) * 8u];
    size_t n = 0u;

    if (value == 0u) {
        log_write_char('0');
        return;
    }

    while (value != 0u) {
        unsigned int digit = value % base;
        digits[n++] = (char)((digit < 10u) ? ('0' + digit) : ('a' + digit - 10u));
        value /= base;
    }

    while (n > 0u) {
        log_write_char(digits[--n]);
    }
}

static void log_write_hex_byte(uint8_t value)
{
    static const char hex[] = "0123456789abcdef";
    log_write_char(hex[(value >> 4) & 0x0fu]);
    log_write_char(hex[value & 0x0fu]);
}

static void log_write_signed(int value)
{
    unsigned int magnitude;
    if (value < 0) {
        log_write_char('-');
        magnitude = 0u - (unsigned int)value;
    } else {
        magnitude = (unsigned int)value;
    }
    log_write_unsigned(magnitude, 10u);
}

void mc_log_vemit(const char *level, const char *fmt, va_list args)
{
    log_write_char('[');
    log_write_str(level);
    log_write_str("] ");

    while (*fmt) {
        char spec;
        if (*fmt != '%') {
            log_write_char(*fmt++);
            continue;
        }

        fmt++;
        spec = *fmt;
        if (spec == '\0') {
            log_write_char('%');
            break;
        }
        fmt++;

        switch (spec) {
        case 's':
            log_write_str(va_arg(args, const char *));
            break;
        case 'u':
            log_write_unsigned(va_arg(args, unsigned int), 10u);
            break;
        case 'd':
            log_write_signed(va_arg(args, int));
            break;
        case 'x':
            log_write_unsigned(va_arg(args, unsigned int), 16u);
            break;
        case 'c':
            log_write_char((char)va_arg(args, int));
            break;
        case '%':
            log_write_char('%');
            break;
        default:
            log_write_char('%');
            log_write_char(spec);
            break;
        }
    }

    log_write_str("\r\n");
}

void mc_log_emit(const char *level, const char *fmt, ...)
{
    va_list args;
    va_start(args, fmt);
    mc_log_vemit(level, fmt, args);
    va_end(args);
}

void mc_log_hexdump(const char *level, const char *label, const uint8_t *data, size_t len)
{
    size_t i;

    mc_log_emit(level, "%s len=%u", label, (unsigned int)len);
    for (i = 0u; i < len; i++) {
        if ((i % 16u) == 0u) {
            log_write_char('[');
            log_write_str(level);
            log_write_str("] ");
            log_write_str(label);
            log_write_str("+");
            log_write_unsigned((unsigned int)i, 10u);
            log_write_str(": ");
        } else {
            log_write_char(' ');
        }

        log_write_hex_byte(data[i]);

        if (((i % 16u) == 15u) || (i + 1u == len)) {
            log_write_str("\r\n");
        }
    }
}
