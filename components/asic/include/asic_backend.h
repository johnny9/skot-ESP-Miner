#ifndef ASIC_BACKEND_H
#define ASIC_BACKEND_H

#include "asic.h"

typedef struct {
    uint8_t (*init)(GlobalState *state);
    task_result *(*process_work)(GlobalState *state);
    int (*set_max_baud)(GlobalState *state);
    void (*send_work)(GlobalState *state, bm_job *job);
    void (*set_version_mask)(GlobalState *state, uint32_t mask);
    void (*set_frequency)(GlobalState *state);
    void (*set_nonce_space)(GlobalState *state);
    double (*get_job_frequency_ms)(GlobalState *state);
    void (*read_registers)(GlobalState *state);
} asic_backend_t;

void ASIC_set_backend(const asic_backend_t *backend);
void ASIC_reset_backend(void);

const asic_backend_t *asic_bitmain_backend(void);

#endif
