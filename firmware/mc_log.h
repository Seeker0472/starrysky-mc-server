#ifndef MC_LOG_H
#define MC_LOG_H

#include "mc_firmware_config.h"
#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>

void mc_log_emit(const char *level, const char *fmt, ...);
void mc_log_vemit(const char *level, const char *fmt, va_list args);
void mc_log_hexdump(const char *level, const char *label, const uint8_t *data, size_t len);

#if MC_LOG_LEVEL >= MC_LOG_INFO
#define MC_LOGI(...) do { mc_log_emit("I", __VA_ARGS__); } while (0)
#else
#define MC_LOGI(...) do { } while (0)
#endif

#if MC_LOG_LEVEL >= MC_LOG_DEBUG
#define MC_LOGD(...) do { mc_log_emit("D", __VA_ARGS__); } while (0)
#else
#define MC_LOGD(...) do { } while (0)
#endif

#if MC_LOG_LEVEL >= MC_LOG_TRACE
#define MC_LOGT(...) do { mc_log_emit("T", __VA_ARGS__); } while (0)
#define MC_LOGT_HEXDUMP(label, data, len) do { mc_log_hexdump("T", (label), (data), (len)); } while (0)
#else
#define MC_LOGT(...) do { } while (0)
#define MC_LOGT_HEXDUMP(label, data, len) do { } while (0)
#endif

#endif
