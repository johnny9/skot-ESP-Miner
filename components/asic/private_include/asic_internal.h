#ifndef ASIC_INTERNAL_H_
#define ASIC_INTERNAL_H_

#include "asic_job.h"

#include "asic_context.h"

/* Takes ownership of the common job and dispatches it to the selected driver. */
void ASIC_send_work(asic_context_t *state, asic_job_t *job);

#endif
