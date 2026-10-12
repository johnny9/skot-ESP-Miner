#include "asic_context_internal.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>

static pthread_mutex_t owner_lock = PTHREAD_MUTEX_INITIALIZER;
static asic_context_t *owner;

bool asic_context_init(asic_context_t *context, const asic_config_t *config)
{
    if (!context || !config || context->initialized ||
        config->model < BM1397 || config->model > BM1373 ||
        !config->asic_count || config->asic_count > 255 || !config->core_count ||
        !isfinite(config->frequency_mhz) || config->frequency_mhz <= 0) return false;
    asic_job_t **jobs = calloc(MAX_ASIC_JOBS, sizeof(*jobs));
    uint8_t *valid = calloc(MAX_ASIC_JOBS, sizeof(*valid));
    if (!jobs || !valid || pthread_mutex_init(&context->valid_jobs_lock, NULL)) {
        free(jobs);
        free(valid);
        return false;
    }
    context->active_jobs = jobs;
    context->valid_jobs = valid;
    context->config = *config;
    context->requested_frequency_mhz = config->frequency_mhz;
    context->actual_frequency_mhz = 0;
    context->hardware_initialized = false;
    context->initialized = true;
    return true;
}

bool asic_context_claim(asic_context_t *context)
{
    pthread_mutex_lock(&owner_lock);
    bool allowed = context && context->initialized && (!owner || owner == context);
    if (allowed) owner = context;
    pthread_mutex_unlock(&owner_lock);
    return allowed;
}

bool asic_context_is_active(const asic_context_t *context)
{
    pthread_mutex_lock(&owner_lock);
    bool active = context && owner == context;
    pthread_mutex_unlock(&owner_lock);
    return active;
}

bool asic_context_has_owner(void)
{
    pthread_mutex_lock(&owner_lock);
    bool active = owner != NULL;
    pthread_mutex_unlock(&owner_lock);
    return active;
}

void asic_context_destroy(asic_context_t *context)
{
    if (!context || !context->initialized) return;
    pthread_mutex_lock(&owner_lock);
    if (owner == context) {
        asic_io_close(ASIC_IO_DATA);
        asic_io_close(ASIC_IO_CONTROL);
        owner = NULL;
    }
    pthread_mutex_unlock(&owner_lock);
    for (size_t i = 0; i < MAX_ASIC_JOBS; ++i) free(context->active_jobs[i]);
    free(context->active_jobs);
    free(context->valid_jobs);
    pthread_mutex_destroy(&context->valid_jobs_lock);
    memset(context, 0, sizeof(*context));
}
