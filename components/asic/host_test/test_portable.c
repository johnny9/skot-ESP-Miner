#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "asic.h"
#include "asic_common.h"
#include "asic_platform.h"
#include "bm_job_packet.h"
#include "crc.h"
#include "serial.h"

typedef struct {
    bool open[2], detect, short_write, fail_read, fail_drain;
    int baud[2];
    int64_t now;
    asic_model_t model;
    uint8_t response[32], packet[160];
    size_t response_size, offset, packet_size, writes;
} fake_io_t;

static void finish_response(fake_io_t *io, size_t size, uint8_t flags)
{
    io->response_size = size;
    io->offset = 0;
    for (unsigned crc = 0; crc < 32; ++crc) {
        io->response[size - 1] = flags | crc;
        if (!crc5(io->response + 2, size - 2)) return;
    }
    assert(false);
}

static asic_err_t fake_open(void *user, asic_io_channel_t c, int baud)
{
    fake_io_t *io = user;
    io->open[c] = true;
    io->baud[c] = baud;
    return ASIC_OK;
}
static void fake_close(void *user, asic_io_channel_t c) { ((fake_io_t *)user)->open[c] = false; }
static bool fake_is_open(void *user, asic_io_channel_t c) { return ((fake_io_t *)user)->open[c]; }
static int fake_read(void *user, asic_io_channel_t c, uint8_t *data, size_t size, uint32_t ms)
{
    (void)c;
    fake_io_t *io = user;
    if (io->fail_read) return -1;
    size_t remaining = io->response_size - io->offset;
    if (!remaining) { io->now += (int64_t)ms * 1000; return 0; }
    if (size > remaining) size = remaining;
    if (size > 2) size = 2;
    memcpy(data, io->response + io->offset, size);
    io->offset += size;
    return (int)size;
}
static int fake_write(void *user, asic_io_channel_t c, const uint8_t *data, size_t size)
{
    (void)c;
    fake_io_t *io = user;
    ++io->writes;
    assert(size <= sizeof(io->packet));
    memcpy(io->packet, data, size);
    io->packet_size = size;
    if (io->short_write) return (int)size - 1;
    if (io->detect && size == 7 && data[2] == 0x52 && data[5] == 0) {
        const uint16_t chip_ids[] = {0x1397, 0x1366, 0x1368, 0x1370, 0x1373};
        uint16_t chip = chip_ids[io->model];
        memset(io->response, 0, sizeof(io->response));
        io->response[0] = 0xaa; io->response[1] = 0x55;
        io->response[2] = chip >> 8; io->response[3] = chip;
        finish_response(io, io->model == BM1397 || io->model == BM1373 ? 9 : 11, 0);
        if (io->model == BM1373) {
            memcpy(io->response + 9, io->response, 9);
            io->response_size = 18;
        }
    }
    return (int)size;
}
static asic_err_t fake_flush(void *user, asic_io_channel_t c)
{
    (void)c;
    fake_io_t *io = user;
    io->offset = io->response_size = 0;
    return ASIC_OK;
}
static asic_err_t fake_drain(void *user, asic_io_channel_t c, uint32_t ms)
{
    (void)c; (void)ms;
    return ((fake_io_t *)user)->fail_drain ? ASIC_ERR_TIMEOUT : ASIC_OK;
}
static int64_t fake_time(void *user) { return ((fake_io_t *)user)->now; }
static void fake_delay(void *user, uint32_t us) { ((fake_io_t *)user)->now += us; }
static void fake_log(void *user, asic_log_level_t level, const char *tag, const char *fmt, va_list args)
{
    (void)user;
    if (level > ASIC_LOG_WARN) return;
    fprintf(stderr, "%s: ", tag);
    vfprintf(stderr, fmt, args);
    fputc('\n', stderr);
}
static const asic_platform_ops_t fake_ops = {
    .open = fake_open, .close = fake_close, .is_open = fake_is_open,
    .read = fake_read, .write = fake_write, .set_baud = fake_open,
    .flush_rx = fake_flush, .drain_tx = fake_drain,
    .time_us = fake_time, .delay_us = fake_delay, .log = fake_log,
};

static void test_transport(void)
{
    assert(SERIAL_init() == ASIC_ERR_NOT_SUPPORTED);
    fake_io_t io = {0};
    asic_platform_ops_t incomplete = fake_ops;
    incomplete.read = NULL;
    assert(!asic_platform_install(&incomplete, &io));
    assert(asic_platform_install(&fake_ops, &io));
    assert(SERIAL_prepare_session(1000000) == ASIC_OK);
    assert(!asic_platform_install(NULL, NULL));
    io.fail_drain = true;
    assert(SERIAL_set_baud(2000000) == ASIC_ERR_TIMEOUT);
    assert(io.baud[0] == 1000000);
    io.fail_drain = false;
    assert(SERIAL_set_baud(2000000) == ASIC_OK);
    uint8_t bytes[8] = {0};
    io.short_write = true;
    assert(SERIAL_send(bytes, sizeof(bytes), false) == -1);
    io.fail_read = true;
    assert(SERIAL_rx(bytes, sizeof(bytes), 50) == -1);
    io.fail_read = false;
    assert(SERIAL_rx(bytes, sizeof(bytes), 50) == 0);
    assert(io.now == 50000);
    assert(SERIAL_rx(bytes, sizeof(bytes), 0) == 0);
    assert(io.now == 50000);
    asic_io_close(ASIC_IO_DATA);
    assert(asic_platform_install(NULL, NULL));
}

static void queue_nonce(fake_io_t *io, uint8_t slot)
{
    memset(io->response, 0, sizeof(io->response));
    io->response[0] = 0xaa; io->response[1] = 0x55;
    io->response[2] = 7;
    io->response[7] = io->model == BM1397 || io->model == BM1366 ? slot : slot << 1;
    if (io->model != BM1397) io->response[9] = 1;
    finish_response(io, io->model == BM1397 ? 9 : 11, 0x80);
}

static void test_driver(asic_model_t model)
{
    fake_io_t io = {.model = model};
    assert(asic_platform_install(&fake_ops, &io));
    assert(SERIAL_init() == ASIC_OK);
    asic_config_t config = {
        .model = model, .asic_count = model == BM1373 ? 2 : 1, .core_count = 128, .small_core_count = 1,
        .difficulty = 256, .frequency_mhz = 200, .software_midstates = 4,
        .default_timeout_ms = 500, .voltage_domains = 1,
        .version_mask = BIP320_VERSION_ROLLING_MASK,
    };
    asic_context_t a = {0}, b = {0};
    assert(asic_context_init(&a, &config));
    assert(!asic_context_init(&a, &config));
    assert(asic_context_init(&b, &config));
    assert(ASIC_init(&a) == 0);
    assert(!a.hardware_initialized);
    size_t writes = io.writes;
    assert(ASIC_init(&b) == 0);
    assert(io.writes == writes);
    io.detect = true;
    assert(ASIC_init(&a) == config.asic_count);
    assert(a.hardware_initialized);
    asic_job_t job = {.version = 0x20000004, .version_mask = BIP320_VERSION_ROLLING_MASK,
                      .ntime = 0x64658bd8, .nbits = 0x1705dd01};
    strcpy(job.job_id, "host-job");
    strcpy(job.extranonce2, "00000000");
    ASIC_send_job(&a, &job);
    assert(io.packet_size == (model == BM1397 ? 152 : 88));
    uint8_t slot = io.packet[4];
    assert(a.valid_jobs[slot]);
    assert(a.active_jobs[slot] != &job);
    memset(&job, 0xa5, sizeof(job));
    assert(strcmp(a.active_jobs[slot]->job_id, "host-job") == 0);
    queue_nonce(&io, slot);
    task_result *result = ASIC_process_work(&a);
    assert(result && result->nonce == 7);
    assert(strcmp(result->job.job_id, "host-job") == 0);
    assert(result->rolled_version == (model == BM1397 ? 0x20000004 : 0x20002004));
    a.active_jobs[slot]->version = 0;
    assert(result->job.version == 0x20000004);
    a.valid_jobs[slot] = 0;
    queue_nonce(&io, slot);
    assert(!ASIC_process_work(&a));
    writes = io.writes;
    ASIC_send_job(&a, &job);
    ASIC_send_job(&b, &(asic_job_t){0});
    assert(io.writes == writes);
    assert(ASIC_set_frequency(&a, NAN) == 0);
    asic_context_destroy(&b);
    assert(io.open[0]);
    assert(!asic_platform_install(NULL, NULL));
    asic_context_destroy(&a);
    assert(!io.open[0]);
    asic_context_destroy(&a);
    assert(asic_platform_install(NULL, NULL));
}

static void test_midstate(void)
{
    // Fixed upstream BM1397 vector, independently calculated by test/verifiers/bm1397.py.
    asic_job_t job = {.version = 0x20000004, .ntime = 0x64658bd8, .nbits = 0x1705dd01};
    const uint8_t prev[] = {0x35, 0xfd, 0x44, 0xbf, 0x83, 0x7b, 0xdc, 0x13, 0xc6, 0xe5, 0x60, 0x7d, 0xb4, 0x72, 0xb5, 0x28, 0xa8, 0x04, 0xd2, 0x48, 0x49, 0x07, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};
    const uint8_t merkle[] = {0xcd, 0x1b, 0xe8, 0x21, 0x32, 0xef, 0x0d, 0x12, 0x05, 0x3d, 0xce, 0xce, 0x1f, 0xa0, 0x24, 0x7f, 0xcf, 0xdb, 0x61, 0xd4, 0xdb, 0xd3, 0xeb, 0x32, 0xea, 0x9e, 0xf9, 0xb4, 0xc6, 0x04, 0xa8, 0x46};
    const uint8_t expected[] = {0x4d, 0x31, 0x85, 0xf2, 0x5f, 0x7d, 0x5d, 0xe0, 0xe3, 0x59, 0x17, 0x41, 0xcb, 0x21, 0xca, 0xe1, 0x15, 0x74, 0xdd, 0xd6, 0x5d, 0x49, 0x0d, 0x3d, 0x68, 0x73, 0x9f, 0x8a, 0x52, 0xea, 0xdf, 0x91};
    memcpy(job.prev_hash, prev, sizeof(prev));
    memcpy(job.merkle_root, merkle, sizeof(merkle));
    bm1397_job_packet_t packet;
    bm1397_build_job_packet(&job, 4, 4, &packet);
    assert(packet.num_midstates == 1);
    assert(memcmp(packet.midstates[0], expected, 32) == 0);
}

int main(void)
{
    test_midstate();
    test_transport();
    for (asic_model_t model = BM1397; model <= BM1373; ++model) test_driver(model);
    puts("Host transport, lifecycle, packets, and all five ASIC drivers passed");
    return 0;
}
