#include "asic_backend.h"

static const asic_backend_t *selected_backend;

static const asic_backend_t *active_backend(void)
{
    return selected_backend != NULL ? selected_backend : asic_bitmain_backend();
}

void ASIC_set_backend(const asic_backend_t *backend)
{
    selected_backend = backend;
}

void ASIC_reset_backend(void)
{
    selected_backend = NULL;
}

uint8_t ASIC_init(GlobalState *state)
{
    return active_backend()->init(state);
}

task_result *ASIC_process_work(GlobalState *state)
{
    return active_backend()->process_work(state);
}

int ASIC_set_max_baud(GlobalState *state)
{
    return active_backend()->set_max_baud(state);
}

void ASIC_send_work(GlobalState *state, bm_job *job)
{
    active_backend()->send_work(state, job);
}

void ASIC_set_version_mask(GlobalState *state, uint32_t mask)
{
    active_backend()->set_version_mask(state, mask);
}

void ASIC_set_frequency(GlobalState *state)
{
    active_backend()->set_frequency(state);
}

void ASIC_set_nonce_space(GlobalState *state)
{
    active_backend()->set_nonce_space(state);
}

double ASIC_get_asic_job_frequency_ms(GlobalState *state)
{
    return active_backend()->get_job_frequency_ms(state);
}

void ASIC_read_registers(GlobalState *state)
{
    active_backend()->read_registers(state);
}
