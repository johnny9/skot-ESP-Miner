#ifndef ASIC_CONTEXT_H
#define ASIC_CONTEXT_H
#include <stdbool.h>
#include <pthread.h>
#include "asic_config.h"

#define MAX_ASIC_JOBS 128

// Zero-initialize before init; never copy a live context or its mutex.
typedef struct asic_context {
    asic_config_t config;
    asic_job_t **active_jobs;
    uint8_t *valid_jobs;
    pthread_mutex_t valid_jobs_lock;
    float requested_frequency_mhz;
    float actual_frequency_mhz;
    bool initialized;
    bool hardware_initialized;
} asic_context_t;

bool asic_context_init(asic_context_t *context, const asic_config_t *config);
// Stop users and power down/reset the board before destruction.
void asic_context_destroy(asic_context_t *context);
#endif
