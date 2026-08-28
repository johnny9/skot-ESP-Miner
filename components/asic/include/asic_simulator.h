#ifndef ASIC_SIMULATOR_H
#define ASIC_SIMULATOR_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "asic_backend.h"
#include "asic_common.h"

#define ASIC_SIMULATOR_RESULT_CAPACITY 8

typedef struct {
    uint8_t chip_count;
    int baud_rate;
    double job_frequency_ms;
    uint32_t version_mask;
    const bm_job *last_job;
    size_t init_calls;
    size_t work_calls;
    size_t frequency_calls;
    size_t nonce_space_calls;
    size_t register_calls;
    size_t queued_results;
} asic_simulator_snapshot_t;

const asic_backend_t *asic_simulator_backend(void);
void asic_simulator_reset(void);
void asic_simulator_configure(uint8_t chip_count, int baud_rate, double job_frequency_ms);
bool asic_simulator_queue_result(const task_result *result);
void asic_simulator_get_snapshot(asic_simulator_snapshot_t *snapshot);

#endif
