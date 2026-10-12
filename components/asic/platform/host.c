#define _POSIX_C_SOURCE 200809L
#include "asic_context_internal.h"
#include <errno.h>
#include <stdio.h>
#include <time.h>

/* The default host backend provides time and logging, but never pretends a
 * physical ASIC is attached. Install transport callbacks before hardware I/O. */
static asic_err_t host_open(void *u, asic_io_channel_t c, int baud)
{ (void)u; (void)c; (void)baud; return ASIC_ERR_NOT_SUPPORTED; }
static void host_close(void *u, asic_io_channel_t c) { (void)u; (void)c; }
static bool host_is_open(void *u, asic_io_channel_t c) { (void)u; (void)c; return false; }
static int host_read(void *u, asic_io_channel_t c, uint8_t *b, size_t n, uint32_t ms)
{ (void)u; (void)c; (void)b; (void)n; (void)ms; return -1; }
static int host_write(void *u, asic_io_channel_t c, const uint8_t *b, size_t n)
{ (void)u; (void)c; (void)b; (void)n; return -1; }
static asic_err_t host_flush(void *u, asic_io_channel_t c)
{ (void)u; (void)c; return ASIC_ERR_NOT_SUPPORTED; }
static asic_err_t host_drain(void *u, asic_io_channel_t c, uint32_t ms)
{ (void)ms; return host_flush(u, c); }
static int64_t host_time(void *u)
{
    (void)u;
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    return (int64_t)now.tv_sec * 1000000 + now.tv_nsec / 1000;
}
static void host_delay(void *u, uint32_t us)
{
    (void)u;
    struct timespec delay = {.tv_sec = us / 1000000, .tv_nsec = (us % 1000000) * 1000};
    while (nanosleep(&delay, &delay) == -1 && errno == EINTR) {}
}
static void host_log(void *u, asic_log_level_t level, const char *tag, const char *fmt, va_list args)
{
    (void)u;
    if (level > ASIC_LOG_INFO) return;
    fprintf(stderr, "%s: ", tag);
    vfprintf(stderr, fmt, args);
    fputc('\n', stderr);
}
const asic_platform_ops_t *asic_platform_default(void)
{
    static const asic_platform_ops_t ops = {
        .open = host_open, .close = host_close, .is_open = host_is_open,
        .read = host_read, .write = host_write, .set_baud = host_open,
        .flush_rx = host_flush, .drain_tx = host_drain,
        .time_us = host_time, .delay_us = host_delay, .log = host_log,
    };
    return &ops;
}
