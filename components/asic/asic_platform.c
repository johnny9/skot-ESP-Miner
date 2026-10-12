#include "asic_platform.h"
#include "asic_context_internal.h"
#include <limits.h>
#include <stdio.h>
#include <string.h>

static const asic_platform_ops_t *installed;
static asic_platform_ops_t custom;
static void *platform_user;
static const asic_platform_ops_t *platform(void)
{
    return installed ? installed : asic_platform_default();
}

bool asic_platform_install(const asic_platform_ops_t *ops, void *user)
{
    /* Installation is a startup/shutdown operation, externally serialized. */
    if (asic_context_has_owner() || asic_io_is_open(ASIC_IO_DATA) ||
        asic_io_is_open(ASIC_IO_CONTROL)) return false;
    if (ops && (!ops->open || !ops->close || !ops->is_open || !ops->read ||
                !ops->write || !ops->set_baud || !ops->flush_rx ||
                !ops->drain_tx || !ops->time_us || !ops->delay_us)) return false;
    if (ops) custom = *ops;
    installed = ops ? &custom : NULL;
    platform_user = ops ? user : NULL;
    return true;
}

static bool valid_channel(asic_io_channel_t channel)
{
    return channel == ASIC_IO_DATA || channel == ASIC_IO_CONTROL;
}
asic_err_t asic_io_open(asic_io_channel_t c, int baud)
{
    if (!valid_channel(c) || baud <= 0) return ASIC_ERR_INVALID_ARG;
    return platform()->open(platform_user, c, baud);
}
void asic_io_close(asic_io_channel_t c)
{
    if (valid_channel(c)) platform()->close(platform_user, c);
}
bool asic_io_is_open(asic_io_channel_t c)
{
    return valid_channel(c) && platform()->is_open(platform_user, c);
}
int asic_io_read(asic_io_channel_t c, uint8_t *data, size_t size, uint32_t ms)
{
    if (!data || !size || size > INT_MAX || !asic_io_is_open(c)) return -1;
    int result = platform()->read(platform_user, c, data, size, ms);
    return result >= 0 && (size_t)result <= size ? result : -1;
}
int asic_io_write(asic_io_channel_t c, const uint8_t *data, size_t size)
{
    if (!data || !size || size > INT_MAX || !asic_io_is_open(c)) return -1;
    int result = platform()->write(platform_user, c, data, size);
    return result == (int)size ? result : -1;
}
asic_err_t asic_io_set_baud(asic_io_channel_t c, int baud)
{
    if (baud <= 0) return ASIC_ERR_INVALID_ARG;
    if (!asic_io_is_open(c)) return ASIC_ERR_INVALID_STATE;
    return platform()->set_baud(platform_user, c, baud);
}
asic_err_t asic_io_flush_rx(asic_io_channel_t c)
{
    if (!asic_io_is_open(c)) return ASIC_ERR_INVALID_STATE;
    return platform()->flush_rx(platform_user, c);
}
asic_err_t asic_io_drain_tx(asic_io_channel_t c, uint32_t ms)
{
    if (!asic_io_is_open(c)) return ASIC_ERR_INVALID_STATE;
    return platform()->drain_tx(platform_user, c, ms);
}
int64_t asic_time_us(void) { return platform()->time_us(platform_user); }
void asic_delay_us(uint32_t us) { platform()->delay_us(platform_user, us); }
void asic_delay_ms(uint32_t ms)
{
    while (ms > UINT32_MAX / 1000) {
        asic_delay_us((UINT32_MAX / 1000) * 1000);
        ms -= UINT32_MAX / 1000;
    }
    asic_delay_us(ms * 1000);
}
void asic_log(asic_log_level_t level, const char *tag, const char *format, ...)
{
    if (!platform()->log) return;
    va_list args;
    va_start(args, format);
    platform()->log(platform_user, level, tag, format, args);
    va_end(args);
}
void asic_log_hex(const char *tag, const void *data, size_t size, asic_log_level_t level)
{
    const uint8_t *bytes = data;
    for (size_t offset = 0; offset < size; offset += 16) {
        char line[16 * 3 + 1];
        size_t count = size - offset < 16 ? size - offset : 16;
        for (size_t i = 0; i < count; ++i) snprintf(line + i * 3, 4, "%02x ", bytes[offset + i]);
        asic_log(level, tag, "%s", line);
    }
}
size_t asic_strlcpy(char *dest, const char *src, size_t capacity)
{
    size_t length = strlen(src);
    if (capacity) {
        size_t copy = length < capacity - 1 ? length : capacity - 1;
        memcpy(dest, src, copy);
        dest[copy] = '\0';
    }
    return length;
}
const char *asic_error_name(asic_err_t error)
{
    switch (error) {
    case ASIC_OK: return "OK";
    case ASIC_FAIL: return "FAIL";
    case ASIC_ERR_NO_MEM: return "NO_MEM";
    case ASIC_ERR_INVALID_ARG: return "INVALID_ARG";
    case ASIC_ERR_INVALID_STATE: return "INVALID_STATE";
    case ASIC_ERR_INVALID_SIZE: return "INVALID_SIZE";
    case ASIC_ERR_NOT_FOUND: return "NOT_FOUND";
    case ASIC_ERR_NOT_SUPPORTED: return "NOT_SUPPORTED";
    case ASIC_ERR_TIMEOUT: return "TIMEOUT";
    case ASIC_ERR_INVALID_RESPONSE: return "INVALID_RESPONSE";
    case ASIC_ERR_INVALID_CRC: return "INVALID_CRC";
    default: return "PLATFORM_ERROR";
    }
}
