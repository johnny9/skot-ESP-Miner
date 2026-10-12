#ifndef BM1397_TEST_HARNESS_H
#define BM1397_TEST_HARNESS_H

#include <stddef.h>
#include <stdint.h>
#include "asic_job.h"
#include "asic_common.h"

#include "asic_context.h"

enum { BM1397_HARNESS_RESPONSE_SIZE = 9 };

typedef struct {
    uint8_t (*init)(asic_context_t *state);
    void (*send_work)(asic_context_t *state, asic_job_t *job);
    task_result *(*process_work)(asic_context_t *state);
} bm1397_harness_driver_t;

typedef struct {
    uint8_t bytes[160];
    size_t length;
} bm1397_harness_packet_t;

extern const bm1397_harness_driver_t bm1397_harness_driver;

asic_context_t *bm1397_harness_begin(void);
void bm1397_harness_end(void);
void bm1397_harness_clear_packets(void);
size_t bm1397_harness_packet_count(void);
const bm1397_harness_packet_t *bm1397_harness_packet(size_t index);
asic_job_t *bm1397_harness_active_job(uint8_t job_id);
void bm1397_harness_install_job(uint8_t job_id, asic_job_t *job);
void bm1397_harness_mark_job_valid(uint8_t job_id);
void bm1397_harness_queue_response(
    const uint8_t response[BM1397_HARNESS_RESPONSE_SIZE]);

#endif
