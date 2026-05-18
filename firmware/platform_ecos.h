#ifndef PLATFORM_ECOS_H
#define PLATFORM_ECOS_H

#include <stddef.h>
#include <stdint.h>

void platform_init(void);
size_t platform_bridge_read(uint8_t *dst, size_t max_len);
size_t platform_bridge_write(const uint8_t *src, size_t max_len);
void platform_log_write(const char *src, size_t len);
uint32_t platform_ticks(void);

size_t platform_uart_read(uint8_t *dst, size_t max_len);
size_t platform_uart_write(const uint8_t *src, size_t max_len);
void platform_log(const char *msg);

#endif
