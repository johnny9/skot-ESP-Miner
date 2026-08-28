#include <string.h>

#include <esp_log.h>

#include "bm1397.h"
#include "bm1366.h"
#include "bm1368.h"
#include "bm1370.h"
#include "bm1373.h"

#include "asic.h"
#include "asic_backend.h"
#include "global_state.h"
#include "mining.h"
#include "device_config.h"
#include "frequency_transition_bmXX.h"

static const char *TAG = "asic";

static uint8_t bitmain_init(GlobalState * GLOBAL_STATE)
{
    ESP_LOGI(TAG, "Initializing %dx %s", GLOBAL_STATE->DEVICE_CONFIG.family.asic_count, GLOBAL_STATE->DEVICE_CONFIG.family.asic.name);
    switch (GLOBAL_STATE->DEVICE_CONFIG.family.asic.id) {
        case BM1397:
            return BM1397_init(GLOBAL_STATE);
        case BM1366:
            return BM1366_init(GLOBAL_STATE);
        case BM1368:
            return BM1368_init(GLOBAL_STATE);
        case BM1370:
            return BM1370_init(GLOBAL_STATE);
        case BM1373:
            return BM1373_init(GLOBAL_STATE);
    }
    ESP_LOGE(TAG, "Unknown ASIC id %d", GLOBAL_STATE->DEVICE_CONFIG.family.asic.id);
    return 0;
}

static task_result *bitmain_process_work(GlobalState * GLOBAL_STATE)
{
    switch (GLOBAL_STATE->DEVICE_CONFIG.family.asic.id) {
        case BM1397:
            return BM1397_process_work(GLOBAL_STATE);
        case BM1366:
            return BM1366_process_work(GLOBAL_STATE);
        case BM1368:
            return BM1368_process_work(GLOBAL_STATE);
        case BM1370:
            return BM1370_process_work(GLOBAL_STATE);
        case BM1373:
            return BM1373_process_work(GLOBAL_STATE);
    }
    ESP_LOGE(TAG, "Unknown ASIC id %d — cannot process work", GLOBAL_STATE->DEVICE_CONFIG.family.asic.id);
    return NULL;
}

static int bitmain_set_max_baud(GlobalState * GLOBAL_STATE)
{
    switch (GLOBAL_STATE->DEVICE_CONFIG.family.asic.id) {
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
    ESP_LOGE(TAG, "Unknown ASIC id %d — cannot set max baud", GLOBAL_STATE->DEVICE_CONFIG.family.asic.id);
    return 0;
}

static void bitmain_send_work(GlobalState * GLOBAL_STATE, bm_job * next_job)
{
    switch (GLOBAL_STATE->DEVICE_CONFIG.family.asic.id) {
        case BM1397:
            BM1397_send_work(GLOBAL_STATE, next_job);
            break;
        case BM1366:
            BM1366_send_work(GLOBAL_STATE, next_job);
            break;
        case BM1368:
            BM1368_send_work(GLOBAL_STATE, next_job);
            break;
        case BM1370:
            BM1370_send_work(GLOBAL_STATE, next_job);
            break;
        case BM1373:
            BM1373_send_work(GLOBAL_STATE, next_job);
            break;
        default:
            ESP_LOGE(TAG, "Unknown ASIC id %d — cannot send work", GLOBAL_STATE->DEVICE_CONFIG.family.asic.id);
            break;
    }
}

static void bitmain_set_version_mask(GlobalState * GLOBAL_STATE, uint32_t mask)
{
    switch (GLOBAL_STATE->DEVICE_CONFIG.family.asic.id) {
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
            ESP_LOGE(TAG, "Unknown ASIC id %d — cannot set version mask", GLOBAL_STATE->DEVICE_CONFIG.family.asic.id);
            break;
    }
}

static void bitmain_set_frequency(GlobalState * GLOBAL_STATE)
{
    switch (GLOBAL_STATE->DEVICE_CONFIG.family.asic.id) {
        case BM1397:
            do_frequency_transition(GLOBAL_STATE, BM1397_send_hash_frequency);
            return;
        case BM1366:
            do_frequency_transition(GLOBAL_STATE, BM1366_send_hash_frequency);
            return;
        case BM1368:
            do_frequency_transition(GLOBAL_STATE, BM1368_send_hash_frequency);
            return;
        case BM1370:
            do_frequency_transition(GLOBAL_STATE, BM1370_send_hash_frequency);
            return;
        case BM1373:
            do_frequency_transition(GLOBAL_STATE, BM1373_send_hash_frequency);
            return;
    }
    ESP_LOGE(TAG, "Unknown ASIC id %d — cannot set frequency", GLOBAL_STATE->DEVICE_CONFIG.family.asic.id);
}

static void bitmain_set_nonce_space(GlobalState * GLOBAL_STATE)
{
    float nonce_percent = 1.0;
    int cores = GLOBAL_STATE->DEVICE_CONFIG.family.asic.core_count;
    int asic_count = GLOBAL_STATE->DEVICE_CONFIG.family.asic_count;
    float frequency = GLOBAL_STATE->POWER_MANAGEMENT_MODULE.actual_frequency;

    switch (GLOBAL_STATE->DEVICE_CONFIG.family.asic.id) {
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
    ESP_LOGE(TAG, "Unknown ASIC id %d — cannot set nonce space", GLOBAL_STATE->DEVICE_CONFIG.family.asic.id);
}

static double bitmain_get_job_frequency_ms(GlobalState * GLOBAL_STATE)
{
    float freq = GLOBAL_STATE->POWER_MANAGEMENT_MODULE.frequency_value;
    int cores = GLOBAL_STATE->DEVICE_CONFIG.family.asic.core_count;
    int small_cores = GLOBAL_STATE->DEVICE_CONFIG.family.asic.small_core_count;
    int asic_count = GLOBAL_STATE->DEVICE_CONFIG.family.asic_count;
    int asic_default_timeout_divided = GLOBAL_STATE->DEVICE_CONFIG.family.asic.default_asic_timeout / _next_power_of_two(asic_count);

    switch (GLOBAL_STATE->DEVICE_CONFIG.family.asic.id) {
        case BM1397:
            // no version-rolling so same Nonce Space is splitted between Big Cores
            return calculate_bm_timeout_ms(freq, asic_count, small_cores, cores, GLOBAL_STATE->DEVICE_CONFIG.family.asic.software_midstates, 1.0, asic_default_timeout_divided);
        case BM1366:
        case BM1368:
        case BM1370:
        case BM1373:
            return asic_default_timeout_divided;
    }
    ESP_LOGE(TAG, "Unknown ASIC id %d — cannot compute job frequency", GLOBAL_STATE->DEVICE_CONFIG.family.asic.id);
    return 500;
}

static void bitmain_read_registers(GlobalState * GLOBAL_STATE)
{
    switch (GLOBAL_STATE->DEVICE_CONFIG.family.asic.id) {
        case BM1397:
            BM1397_read_registers();
            break;
        case BM1366:
            BM1366_read_registers();
            break;
        case BM1368:
            BM1368_read_registers();
            break;
        case BM1370:
            BM1370_read_registers();
            break;
        case BM1373:
            BM1373_read_registers();
            break;
        default:
            ESP_LOGE(TAG, "Unknown ASIC id %d — cannot read registers", GLOBAL_STATE->DEVICE_CONFIG.family.asic.id);
            break;
    }
}

static const asic_backend_t bitmain_backend = {
    .init = bitmain_init,
    .process_work = bitmain_process_work,
    .set_max_baud = bitmain_set_max_baud,
    .send_work = bitmain_send_work,
    .set_version_mask = bitmain_set_version_mask,
    .set_frequency = bitmain_set_frequency,
    .set_nonce_space = bitmain_set_nonce_space,
    .get_job_frequency_ms = bitmain_get_job_frequency_ms,
    .read_registers = bitmain_read_registers,
};

const asic_backend_t *asic_bitmain_backend(void)
{
    return &bitmain_backend;
}
