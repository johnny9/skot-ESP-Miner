#ifndef ASIC_PLATFORM_H
#define ASIC_PLATFORM_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdarg.h>
#include "asic_err.h"

typedef enum { ASIC_IO_DATA, ASIC_IO_CONTROL } asic_io_channel_t;
typedef enum { ASIC_LOG_ERROR, ASIC_LOG_WARN, ASIC_LOG_INFO, ASIC_LOG_DEBUG } asic_log_level_t;

/* Install once before starting any driver or board operation. Callbacks and
 * user data must outlive all users. Host applications may implement USB/TTY,
 * Bitaxe Raw, or scripted I/O here. read returns 0 on timeout, -1 on failure;
 * write must consume the complete buffer or fail. timeout_ms=0 is a poll.
 * Clock is monotonic microseconds; tests may supply a virtual clock.
 */
typedef struct {
    asic_err_t (*open)(void *, asic_io_channel_t, int baud);
    void (*close)(void *, asic_io_channel_t);
    bool (*is_open)(void *, asic_io_channel_t);
    int (*read)(void *, asic_io_channel_t, uint8_t *, size_t, uint32_t timeout_ms);
    int (*write)(void *, asic_io_channel_t, const uint8_t *, size_t);
    asic_err_t (*set_baud)(void *, asic_io_channel_t, int);
    asic_err_t (*flush_rx)(void *, asic_io_channel_t);
    asic_err_t (*drain_tx)(void *, asic_io_channel_t, uint32_t timeout_ms);
    int64_t (*time_us)(void *);
    void (*delay_us)(void *, uint32_t);
    void (*log)(void *, asic_log_level_t, const char *, const char *, va_list);
} asic_platform_ops_t;

bool asic_platform_install(const asic_platform_ops_t *ops, void *user);
asic_err_t asic_io_open(asic_io_channel_t channel, int baud);
void asic_io_close(asic_io_channel_t channel);
bool asic_io_is_open(asic_io_channel_t channel);
int asic_io_read(asic_io_channel_t channel, uint8_t *data, size_t size, uint32_t timeout_ms);
int asic_io_write(asic_io_channel_t channel, const uint8_t *data, size_t size);
asic_err_t asic_io_set_baud(asic_io_channel_t channel, int baud);
asic_err_t asic_io_flush_rx(asic_io_channel_t channel);
asic_err_t asic_io_drain_tx(asic_io_channel_t channel, uint32_t timeout_ms);
int64_t asic_time_us(void);
void asic_delay_us(uint32_t duration);
void asic_delay_ms(uint32_t duration);
void asic_log(asic_log_level_t level, const char *tag, const char *format, ...)
    __attribute__((format(printf, 3, 4)));
void asic_log_hex(const char *tag, const void *data, size_t size, asic_log_level_t level);
size_t asic_strlcpy(char *dest, const char *src, size_t capacity);

#define ASIC_LOGE(tag, ...) asic_log(ASIC_LOG_ERROR, tag, __VA_ARGS__)
#define ASIC_LOGW(tag, ...) asic_log(ASIC_LOG_WARN, tag, __VA_ARGS__)
#define ASIC_LOGI(tag, ...) asic_log(ASIC_LOG_INFO, tag, __VA_ARGS__)
#define ASIC_LOGD(tag, ...) asic_log(ASIC_LOG_DEBUG, tag, __VA_ARGS__)
#define ASIC_LOG_BUFFER_HEX(tag, data, size) asic_log_hex(tag, data, size, ASIC_LOG_DEBUG)
#define ASIC_LOG_BUFFER_HEX_LEVEL(tag, data, size, level) asic_log_hex(tag, data, size, level)
#define ASIC_RETURN_ON_ERROR(expr, tag, ...) do { \
    asic_err_t asic_return_error = (expr); \
    if (asic_return_error != ASIC_OK) { \
        ASIC_LOGE(tag, __VA_ARGS__); return asic_return_error; \
    } \
} while (0)

#endif
