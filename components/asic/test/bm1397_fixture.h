#ifndef BM1397_FIXTURE_H
#define BM1397_FIXTURE_H

#include <stddef.h>
#include <stdint.h>
#include "asic_common.h"

typedef struct GlobalState GlobalState;
typedef struct bm_job bm_job;

enum { BM1397_FIXTURE_RESPONSE_SIZE = 9 };

typedef struct {
    uint8_t (*init)(GlobalState *state);
    void (*send_work)(GlobalState *state, bm_job *job);
    task_result *(*process_work)(GlobalState *state);
} bm1397_fixture_driver_t;

typedef struct {
    uint8_t bytes[160];
    size_t length;
} bm1397_fixture_packet_t;

extern const bm1397_fixture_driver_t bm1397_fixture_driver;

GlobalState *bm1397_fixture_begin(void);
void bm1397_fixture_end(void);
void bm1397_fixture_clear_packets(void);
size_t bm1397_fixture_packet_count(void);
const bm1397_fixture_packet_t *bm1397_fixture_packet(size_t index);
bm_job *bm1397_fixture_active_job(uint8_t job_id);
void bm1397_fixture_queue_response(
    const uint8_t response[BM1397_FIXTURE_RESPONSE_SIZE]);

#endif
