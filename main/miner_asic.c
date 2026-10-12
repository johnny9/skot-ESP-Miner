#include "miner_asic.h"
#include "global_state.h"
#include "asic.h"

esp_err_t miner_asic_get_domain_measurement(GlobalState * GLOBAL_STATE, uint8_t asic_nr,
                                      uint8_t domain_nr, asic_domain_measurement_t * measurement)
{
    if (GLOBAL_STATE == NULL || measurement == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    HashrateMonitorModule * monitor = &GLOBAL_STATE->HASHRATE_MONITOR_MODULE;
    if (!monitor->is_initialized) {
        return ESP_ERR_INVALID_STATE;
    }

    if (asic_nr >= GLOBAL_STATE->DEVICE_CONFIG.family.asic_count ||
        domain_nr >= GLOBAL_STATE->DEVICE_CONFIG.family.asic.hash_domains) {
        return ESP_ERR_INVALID_ARG;
    }

    pthread_mutex_lock(&monitor->lock);
    measurement_t register_measurement = monitor->domain_measurements[asic_nr][domain_nr];
    pthread_mutex_unlock(&monitor->lock);

    measurement->time_us = register_measurement.time_us;
    measurement->hashrate = register_measurement.hashrate;
    return ESP_OK;
}

bool miner_asic_context_init(GlobalState *state)
{
    const AsicConfig *chip = &state->DEVICE_CONFIG.family.asic;
    const asic_config_t config = {
        .model = chip->id, .asic_count = state->DEVICE_CONFIG.family.asic_count,
        .core_count = chip->core_count, .small_core_count = chip->small_core_count,
        .difficulty = chip->difficulty, .default_timeout_ms = chip->default_asic_timeout,
        .software_midstates = chip->software_midstates,
        .voltage_domains = state->DEVICE_CONFIG.family.voltage_domains,
        .frequency_mhz = chip->default_frequency_mhz,
        .version_mask = BIP320_VERSION_ROLLING_MASK,
    };
    return asic_context_init(&state->asic, &config);
}

uint8_t miner_asic_init(GlobalState *state)
{
    state->asic.requested_frequency_mhz = state->POWER_MANAGEMENT_MODULE.frequency_value;
    state->asic.actual_frequency_mhz = state->POWER_MANAGEMENT_MODULE.actual_frequency;
    uint8_t count = ASIC_init(&state->asic);
    state->POWER_MANAGEMENT_MODULE.actual_frequency = state->asic.actual_frequency_mhz;
    return count;
}

void miner_asic_set_frequency(GlobalState *state)
{
    state->POWER_MANAGEMENT_MODULE.actual_frequency = ASIC_set_frequency(
        &state->asic, state->POWER_MANAGEMENT_MODULE.frequency_value);
}
