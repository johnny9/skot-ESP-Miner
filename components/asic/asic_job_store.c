#include "asic.h"
#include "global_state.h"

bool ASIC_get_job_snapshot(GlobalState *state, uint8_t job_id, asic_job_t *snapshot)
{
    if (state == NULL || snapshot == NULL || job_id >= MAX_ASIC_JOBS ||
        state->ASIC_TASK_MODULE.active_jobs == NULL ||
        state->ASIC_TASK_MODULE.valid_jobs == NULL) {
        return false;
    }
    bool copied = false;
    pthread_mutex_lock(&state->ASIC_TASK_MODULE.valid_jobs_lock);
    const asic_job_t *job = state->ASIC_TASK_MODULE.active_jobs[job_id];
    if (state->ASIC_TASK_MODULE.valid_jobs[job_id] != 0 && job != NULL) {
        *snapshot = *job;
        copied = true;
    }
    pthread_mutex_unlock(&state->ASIC_TASK_MODULE.valid_jobs_lock);
    return copied;
}
