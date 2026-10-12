#include "asic.h"
#include "asic_internal.h"
#include "asic_platform.h"
#include <stdlib.h>
#include <string.h>

void ASIC_send_job(asic_context_t *state, const asic_job_t *job)
{
    if (job == NULL ||
        memchr(job->job_id, 0, sizeof(job->job_id)) == NULL ||
        memchr(job->extranonce2, 0, sizeof(job->extranonce2)) == NULL) {
        ASIC_LOGE("asic", "Invalid common job metadata");
        return;
    }
    asic_job_t *retained_job = malloc(sizeof(*retained_job));
    if (retained_job == NULL) {
        ASIC_LOGE("asic", "Failed to allocate common job");
        return;
    }
    *retained_job = *job;
    ASIC_send_work(state, retained_job);
}
