#include "bonanza_vcore.h"
#include <math.h>
#include <string.h>
#include "esp_check.h"
#include "esp_log.h"
#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "bzm_bridge.h"
#include "bzm_power.h"
#include "global_state.h"
#define GPIO_ASIC_ENABLE CONFIG_GPIO_ASIC_ENABLE
#define GPIO_TPS546_PGOOD 11
static const char *TAG = "bonanza_vcore";
esp_err_t BONANZA_VCORE_init(GlobalState *GLOBAL_STATE)
{
    BONANZA_TPS546_CONFIG config = {0};
    const bzm_tps546_profile_t *profile = &BZM_TPS546_BIRDS_PROFILE;
    config.BONANZA_TPS546_EXTENDED_CONFIG = true;
    config.BONANZA_TPS546_INIT_PHASE = profile->phase;
    memcpy(config.BONANZA_TPS546_INIT_SMBALERT_MASK, profile->smbalert_mask,
           sizeof(config.BONANZA_TPS546_INIT_SMBALERT_MASK));
    config.BONANZA_TPS546_INIT_FREQUENCY = profile->frequency_switch_khz;
    config.BONANZA_TPS546_INIT_VIN_ON = profile->vin_on;
    config.BONANZA_TPS546_INIT_VIN_OFF = profile->vin_off;
    config.BONANZA_TPS546_INIT_VIN_UV_WARN_LIMIT = profile->vin_uv_warn_limit;
    config.BONANZA_TPS546_INIT_VIN_OV_FAULT_LIMIT = profile->vin_ov_fault_limit;
    config.BONANZA_TPS546_INIT_SCALE_LOOP = profile->vout_scale_loop;
    config.BONANZA_TPS546_INIT_VOUT_MIN = profile->vout_min;
    config.BONANZA_TPS546_INIT_VOUT_MAX = profile->vout_max;
    config.BONANZA_TPS546_INIT_VOUT_COMMAND = profile->vout_command;
    config.BONANZA_TPS546_INIT_IOUT_OC_WARN_LIMIT = profile->iout_oc_warn_limit;
    config.BONANZA_TPS546_INIT_IOUT_OC_FAULT_LIMIT = profile->iout_oc_fault_limit;
    config.BONANZA_TPS546_INIT_STACK_CONFIG = profile->stack_config;
    config.BONANZA_TPS546_INIT_SYNC_CONFIG = profile->sync_config;
    config.BONANZA_TPS546_INIT_INTERLEAVE = profile->interleave;
    config.BONANZA_TPS546_INIT_MISC_OPTIONS = profile->misc_options;
    config.BONANZA_TPS546_INIT_PIN_DETECT_OVERRIDE =
        profile->pin_detect_override;
    memcpy(config.BONANZA_TPS546_INIT_COMPENSATION_CONFIG,
           profile->compensation_config,
           sizeof(config.BONANZA_TPS546_INIT_COMPENSATION_CONFIG));
    config.BONANZA_TPS546_INIT_POWER_STAGE_CONFIG = profile->power_stage_config;
    memcpy(config.BONANZA_TPS546_INIT_TELEMETRY_CONFIG,
           profile->telemetry_config,
           sizeof(config.BONANZA_TPS546_INIT_TELEMETRY_CONFIG));
    config.BONANZA_TPS546_INIT_VOUT_TRIM = profile->vout_trim;
    config.BONANZA_TPS546_INIT_VOUT_TRANSITION_RATE =
        profile->vout_transition_rate;
    config.BONANZA_TPS546_INIT_IOUT_CAL_GAIN = profile->iout_cal_gain;
    config.BONANZA_TPS546_INIT_IOUT_CAL_OFFSET = profile->iout_cal_offset;
    config.BONANZA_TPS546_EXT_VOUT_MARGIN_HIGH = profile->vout_margin_high;
    config.BONANZA_TPS546_EXT_VOUT_MARGIN_LOW = profile->vout_margin_low;
    config.BONANZA_TPS546_EXT_VOUT_OV_FAULT_LIMIT =
        profile->vout_ov_fault_limit;
    config.BONANZA_TPS546_EXT_VOUT_OV_FAULT_RESPONSE =
        profile->vout_ov_fault_response;
    config.BONANZA_TPS546_EXT_VOUT_OV_WARN_LIMIT =
        profile->vout_ov_warn_limit;
    config.BONANZA_TPS546_EXT_VOUT_UV_WARN_LIMIT =
        profile->vout_uv_warn_limit;
    config.BONANZA_TPS546_EXT_VOUT_UV_FAULT_LIMIT =
        profile->vout_uv_fault_limit;
    config.BONANZA_TPS546_EXT_VOUT_UV_FAULT_RESPONSE =
        profile->vout_uv_fault_response;
    config.BONANZA_TPS546_EXT_IOUT_OC_FAULT_RESPONSE =
        profile->iout_oc_fault_response;
    config.BONANZA_TPS546_EXT_OT_FAULT_LIMIT = profile->ot_fault_limit;
    config.BONANZA_TPS546_EXT_OT_FAULT_RESPONSE = profile->ot_fault_response;
    config.BONANZA_TPS546_EXT_OT_WARN_LIMIT = profile->ot_warn_limit;
    config.BONANZA_TPS546_EXT_VIN_OV_FAULT_RESPONSE =
        profile->vin_ov_fault_response;
    config.BONANZA_TPS546_EXT_TON_DELAY = profile->ton_delay;
    config.BONANZA_TPS546_EXT_TON_RISE = profile->ton_rise;
    config.BONANZA_TPS546_EXT_TON_MAX_FAULT_LIMIT =
        profile->ton_max_fault_limit;
    config.BONANZA_TPS546_EXT_TON_MAX_FAULT_RESPONSE =
        profile->ton_max_fault_response;
    config.BONANZA_TPS546_EXT_TOFF_DELAY = profile->toff_delay;
    config.BONANZA_TPS546_EXT_TOFF_FALL = profile->toff_fall;
    gpio_config_t enable = {
        .pin_bit_mask = 1ULL << GPIO_ASIC_ENABLE,
        .mode = GPIO_MODE_OUTPUT,
    };
    ESP_RETURN_ON_ERROR(gpio_config(&enable), TAG,
                        "TPS546 enable GPIO config failed");
    ESP_RETURN_ON_ERROR(gpio_set_level(GPIO_ASIC_ENABLE, 0), TAG,
                        "TPS546 enable safe-state failed");
    gpio_config_t pgood = {
        .pin_bit_mask = 1ULL << GPIO_TPS546_PGOOD,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
    };
    ESP_RETURN_ON_ERROR(gpio_config(&pgood), TAG,
                        "TPS546 PGOOD GPIO config failed");
    esp_err_t bridge_err = BZM_bridge_init();
    if (bridge_err == ESP_OK) {
        bridge_err = BZM_bridge_set_5v_enabled(false);
    }
    if (bridge_err != ESP_OK) {
        /*
         * Keep initializing the independently controlled TPS546 in its
         * off state. A blank RP2040 must not prevent Wi-Fi/HTTP recovery,
         * and power management will refuse to mine without the
         * missing bridge readback.
         */
        ESP_LOGE(TAG,
                 "Bonanza bridge safe-state unavailable: %s; "
                 "keeping the TPS546 off for HTTP bridge recovery",
                 esp_err_to_name(bridge_err));
    }
    return BONANZA_TPS546_init(config);
}
static esp_err_t bonanza_set_5v_enabled(void *context, bool enabled)
{
    (void)context;
    return BZM_bridge_set_5v_enabled(enabled);
}

static esp_err_t bonanza_set_regulator_enabled(void *context, bool enabled)
{
    (void)context;
    return gpio_set_level(GPIO_ASIC_ENABLE, enabled ? 1 : 0);
}

static esp_err_t bonanza_set_vout(void *context, float volts)
{
    GlobalState *state = context;
    if (volts != 0.0f &&
        !bzm_power_runtime_voltage_is_allowed(volts)) {
        ESP_LOGE(TAG,
                 "Bitaxe 1002 TPS rail request %.3fV is outside 2.1..3.2V",
                 volts);
        return ESP_ERR_INVALID_ARG;
    }
    return state->DEVICE_CONFIG.TPS546 ? BONANZA_TPS546_set_vout(volts)
                                       : ESP_ERR_INVALID_STATE;
}

static esp_err_t bonanza_validate_power(void *context, float expected_vout)
{
    (void)context;
    if (!bzm_power_runtime_voltage_is_allowed(expected_vout) ||
        gpio_get_level(GPIO_TPS546_PGOOD) == 0) {
        ESP_LOGE(TAG, "TPS546 PGOOD remained low");
        return ESP_FAIL;
    }

    BONANZA_TPS546_StatusSnapshot snapshot;
    ESP_RETURN_ON_ERROR(BONANZA_TPS546_snapshot_status(&snapshot), TAG,
                        "TPS546 status/telemetry read failed");
    if ((snapshot.status_word &
         (BONANZA_TPS546_STATUS_VOUT | BONANZA_TPS546_STATUS_IOUT |
          BONANZA_TPS546_STATUS_INPUT | BONANZA_TPS546_STATUS_PGOOD |
          BONANZA_TPS546_STATUS_OFF | BONANZA_TPS546_STATUS_TEMP |
          BONANZA_TPS546_STATUS_CML)) != 0 ||
        (snapshot.operation & 0x80) == 0 ||
        !isfinite(snapshot.vout_command) ||
        fabsf(snapshot.vout_command - expected_vout) >
            BZM_TPS546_VOUT_READBACK_TOLERANCE_V ||
        snapshot.read_vin < BZM_TPS546_BIRDS_PROFILE.vin_off ||
        snapshot.read_vout <
            expected_vout - BZM_TPS546_VOUT_OPERATING_TOLERANCE_V ||
        snapshot.read_vout >
            expected_vout + BZM_TPS546_VOUT_OPERATING_TOLERANCE_V ||
        snapshot.read_temp1 >= BZM_TPS546_BIRDS_PROFILE.ot_fault_limit) {
        BONANZA_TPS546_log_snapshot(&snapshot);
        ESP_LOGE(TAG,
                 "TPS546 %.3fV command/status/telemetry validation failed",
                 expected_vout);
        return ESP_FAIL;
    }
    return ESP_OK;
}

static void bonanza_power_delay(void *context, uint32_t delay_ms)
{
    (void)context;
    vTaskDelay(pdMS_TO_TICKS(delay_ms));
}

static const bzm_power_ops_t BONANZA_POWER_OPS = {
    .set_5v_enabled = bonanza_set_5v_enabled,
    .set_regulator_enabled = bonanza_set_regulator_enabled,
    .set_vout = bonanza_set_vout,
    .validate_power = bonanza_validate_power,
    .delay_ms = bonanza_power_delay,
};

esp_err_t BONANZA_VCORE_bzm_set_rail_enabled(GlobalState *GLOBAL_STATE, bool enabled)
{
    if (GLOBAL_STATE == NULL ||
        !(GLOBAL_STATE->DEVICE_CONFIG.family.id == BONANZA) ||
        !GLOBAL_STATE->DEVICE_CONFIG.TPS546) {
        return ESP_ERR_INVALID_STATE;
    }
    return bzm_power_set_rail_enabled(
        &BONANZA_POWER_OPS, GLOBAL_STATE, enabled);
}

esp_err_t BONANZA_VCORE_bzm_set_runtime_voltage(GlobalState *GLOBAL_STATE,
                                        float volts)
{
    if (GLOBAL_STATE == NULL ||
        !(GLOBAL_STATE->DEVICE_CONFIG.family.id == BONANZA) ||
        !GLOBAL_STATE->DEVICE_CONFIG.TPS546) {
        return ESP_ERR_INVALID_STATE;
    }
    return bzm_power_set_runtime_voltage(
        &BONANZA_POWER_OPS, GLOBAL_STATE, volts);
}

esp_err_t BONANZA_VCORE_bzm_force_regulator_off(GlobalState *GLOBAL_STATE)
{
    if (GLOBAL_STATE == NULL ||
        !(GLOBAL_STATE->DEVICE_CONFIG.family.id == BONANZA) ||
        !GLOBAL_STATE->DEVICE_CONFIG.TPS546) {
        return ESP_ERR_INVALID_STATE;
    }

    /*
     * This recovery-only primitive deliberately does not contact the RP2040.
     * Drop the ESP-owned hardware enable first, then command the TPS546 off.
     */
    esp_err_t gpio_err = gpio_set_level(GPIO_ASIC_ENABLE, 0);
    esp_err_t tps_err = BONANZA_TPS546_set_vout(0.0f);
    return gpio_err != ESP_OK ? gpio_err : tps_err;
}

esp_err_t BONANZA_VCORE_bzm_snapshot(BONANZA_TPS546_StatusSnapshot *snapshot, bool *pgood)
{
    if (snapshot == NULL || pgood == NULL) return ESP_ERR_INVALID_ARG;
    *pgood = gpio_get_level(GPIO_TPS546_PGOOD) != 0;
    return BONANZA_TPS546_snapshot_status(snapshot);
}
