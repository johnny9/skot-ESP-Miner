#include "asic_internal.h"
#include "asic_context_internal.h"
#include <math.h>
#include <string.h>
#include <stdlib.h>

#include "asic_platform.h"

#include "bm1397.h"
#include "bm1366.h"
#include "bm1368.h"
#include "bm1370.h"
#include "bm1373.h"

#include "asic.h"
#include "asic_context.h"
#include "frequency_transition_bmXX.h"

static const char *TAG = "asic";

static uint8_t initialize_driver(asic_context_t * state)
{
    ASIC_LOGI(TAG, "Initializing %dx ASIC model %d", state->config.asic_count, state->config.model);
    switch (state->config.model) {
        case BM1397:
            return BM1397_init(state);
        case BM1366:
            return BM1366_init(state);
        case BM1368:
            return BM1368_init(state);
        case BM1370:
            return BM1370_init(state);
        case BM1373:
            return BM1373_init(state);
    }
    ASIC_LOGE(TAG, "Unknown ASIC id %d", state->config.model);
    return 0;
}

uint8_t ASIC_init(asic_context_t *state)
{
    if (!asic_context_claim(state)) return 0;
    state->hardware_initialized = false;
    uint8_t count = initialize_driver(state);
    state->hardware_initialized = count != 0;
    return count;
}

task_result * ASIC_process_work(asic_context_t * state)
{
    if (!asic_context_is_active(state) || !state->hardware_initialized) { return NULL; }
    switch (state->config.model) {
        case BM1397:
            return BM1397_process_work(state);
        case BM1366:
            return BM1366_process_work(state);
        case BM1368:
            return BM1368_process_work(state);
        case BM1370:
            return BM1370_process_work(state);
        case BM1373:
            return BM1373_process_work(state);
    }
    ASIC_LOGE(TAG, "Unknown ASIC id %d — cannot process work", state->config.model);
    return NULL;
}

int ASIC_set_max_baud(asic_context_t * state)
{
    if (!asic_context_is_active(state) || !state->hardware_initialized) { return 0; }
    switch (state->config.model) {
        case BM1397:
            return BM1397_set_max_baud();
        case BM1366:
            return BM1366_set_max_baud();
        case BM1368:
            return BM1368_set_max_baud();
        case BM1370:
            return BM1370_set_max_baud();
        case BM1373:
            return BM1373_set_max_baud();
    }
    ASIC_LOGE(TAG, "Unknown ASIC id %d — cannot set max baud", state->config.model);
    return 0;
}

void ASIC_send_work(asic_context_t * state, asic_job_t * next_job)
{
    if (!asic_context_is_active(state) || !state->hardware_initialized) { free(next_job); return; }
    switch (state->config.model) {
        case BM1397:
            BM1397_send_work(state, next_job);
            break;
        case BM1366:
            BM1366_send_work(state, next_job);
            break;
        case BM1368:
            BM1368_send_work(state, next_job);
            break;
        case BM1370:
            BM1370_send_work(state, next_job);
            break;
        case BM1373:
            BM1373_send_work(state, next_job);
            break;
        default:
            ASIC_LOGE(TAG, "Unknown ASIC id %d — cannot send work", state->config.model);
            free(next_job);
            break;
    }
}

void ASIC_set_version_mask(asic_context_t * state, uint32_t mask)
{
    if (!asic_context_is_active(state) || !state->hardware_initialized) { return; }
    switch (state->config.model) {
        case BM1397:
            BM1397_set_version_mask(mask);
            break;
        case BM1366:
            BM1366_set_version_mask(mask);
            break;
        case BM1368:
            BM1368_set_version_mask(mask);
            break;
        case BM1370:
            BM1370_set_version_mask(mask);
            break;
        case BM1373:
            BM1373_set_version_mask(mask);
            break;
        default:
            ASIC_LOGE(TAG, "Unknown ASIC id %d — cannot set version mask", state->config.model);
            break;
    }
}

static void set_frequency(asic_context_t * state)
{
    if (!asic_context_is_active(state) || !state->hardware_initialized) { return; }
    switch (state->config.model) {
        case BM1397:
            do_frequency_transition(state, BM1397_send_hash_frequency);
            return;
        case BM1366:
            do_frequency_transition(state, BM1366_send_hash_frequency);
            return;
        case BM1368:
            do_frequency_transition(state, BM1368_send_hash_frequency);
            return;
        case BM1370:
            do_frequency_transition(state, BM1370_send_hash_frequency);
            return;
        case BM1373:
            do_frequency_transition(state, BM1373_send_hash_frequency);
            return;
    }
    ASIC_LOGE(TAG, "Unknown ASIC id %d — cannot set frequency", state->config.model);
}

float ASIC_set_frequency(asic_context_t *state, float frequency_mhz)
{
    if (!asic_context_is_active(state) || !state->hardware_initialized ||
        !isfinite(frequency_mhz) || frequency_mhz <= 0) return 0;
    state->requested_frequency_mhz = frequency_mhz;
    set_frequency(state);
    return state->actual_frequency_mhz;
}

void ASIC_set_nonce_space(asic_context_t * state)
{
    if (!asic_context_is_active(state) || !state->hardware_initialized) { return; }
    float nonce_percent = 1.0;
    int cores = state->config.core_count;
    int asic_count = state->config.asic_count;
    float frequency = state->actual_frequency_mhz;

    switch (state->config.model) {
        case BM1397:
            return;
        case BM1366:
            BM1366_set_nonce_space(nonce_percent, frequency, asic_count, cores);
            return;
        case BM1368:
            BM1368_set_nonce_space(nonce_percent, frequency, asic_count, cores);
            return;
        case BM1370:
            BM1370_set_nonce_space(nonce_percent, frequency, asic_count, cores);
            return;
        case BM1373:
            BM1373_set_nonce_space(nonce_percent, frequency, asic_count, cores);
            return;
    }
    ASIC_LOGE(TAG, "Unknown ASIC id %d — cannot set nonce space", state->config.model);
}

double ASIC_get_asic_job_frequency_ms(asic_context_t * state)
{
    if (!state || !state->initialized) return 500;
    float freq = state->requested_frequency_mhz;
    int cores = state->config.core_count;
    int small_cores = state->config.small_core_count;
    int asic_count = state->config.asic_count;
    int asic_default_timeout_divided = state->config.default_timeout_ms / _next_power_of_two(asic_count);

    switch (state->config.model) {
        case BM1397:
            // no version-rolling so same Nonce Space is splitted between Big Cores
            return calculate_bm_timeout_ms(freq, asic_count, small_cores, cores, state->config.software_midstates, 1.0, asic_default_timeout_divided);
        case BM1366:
        case BM1368:
        case BM1370:
        case BM1373:
            return asic_default_timeout_divided;
    }
    ASIC_LOGE(TAG, "Unknown ASIC id %d — cannot compute job frequency", state->config.model);
    return 500;
}

void ASIC_read_registers(asic_context_t * state)
{
    if (!asic_context_is_active(state) || !state->hardware_initialized) { return; }
    switch (state->config.model) {
        case BM1397:
            BM1397_read_registers(state);
            break;
        case BM1366:
            BM1366_read_registers(state);
            break;
        case BM1368:
            BM1368_read_registers(state);
            break;
        case BM1370:
            BM1370_read_registers(state);
            break;
        case BM1373:
            BM1373_read_registers();
            break;
        default:
            ASIC_LOGE(TAG, "Unknown ASIC id %d — cannot read registers", state->config.model);
            break;
    }
}
