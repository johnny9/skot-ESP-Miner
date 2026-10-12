#ifndef ASIC_H
#define ASIC_H

#include "asic_err.h"
#include <stdint.h>
#include <stdbool.h>
#include "asic_job.h"

#include "asic_context.h"
typedef struct task_result task_result;

uint8_t ASIC_init(asic_context_t * state);
/* Returns driver-owned storage valid until the next call, or NULL when no
 * usable response is available. Non-register results include the matched job
 * snapshot, independent of subsequent active-slot reuse. */
task_result * ASIC_process_work(asic_context_t * state);
int ASIC_set_max_baud(asic_context_t * state);
/* The caller retains ownership and may release the job after this call. */
void ASIC_send_job(asic_context_t *state, const asic_job_t *job);
void ASIC_set_version_mask(asic_context_t * state, uint32_t mask);
float ASIC_set_frequency(asic_context_t *state, float frequency_mhz);
void ASIC_set_nonce_space(asic_context_t * state);
double ASIC_get_asic_job_frequency_ms(asic_context_t * state);
void ASIC_read_registers(asic_context_t * state);

#endif // ASIC_H
