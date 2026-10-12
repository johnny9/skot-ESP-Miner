#ifndef BM1366_H_
#define BM1366_H_

#include "asic_common.h"
#include "asic_job.h"

#include "asic_context.h"

#define BM1366_SERIALTX_DEBUG false
#define BM1366_SERIALRX_DEBUG false
#define BM1366_DEBUG_WORK false //causes insane amount of debug output
#define BM1366_DEBUG_JOBS false //causes insane amount of debug output

uint8_t BM1366_init(asic_context_t * state);
void BM1366_send_work(asic_context_t * state, asic_job_t * next_job);
void BM1366_set_version_mask(uint32_t version_mask);
int BM1366_set_max_baud(void);
float BM1366_send_hash_frequency(float frequency);
task_result * BM1366_process_work(asic_context_t * state);
void BM1366_read_registers(asic_context_t * state);
void BM1366_set_nonce_space(double nonce_percent, float frequency, uint16_t asic_count, uint16_t cores);

#endif /* BM1366_H_ */
