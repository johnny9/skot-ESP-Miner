#include "bzm_power.h"

#include <math.h>
#include <stddef.h>

#include "bzm_frequency.h"

enum {
    BZM_TUNING_BASE_VOLTAGE_MV = 2800,
    BZM_TUNING_CALIBRATION_VOLTAGE_MV = 50,
    BZM_TUNING_AMBIENT_OFFSET_MV = 50,
};

static const float BZM_TUNING_CALIBRATION_FREQUENCY_MHZ = 25.0f;
static const float BZM_TUNING_LOW_FREQUENCY_MHZ = 1000.0f;
static const float BZM_TUNING_BALANCED_FREQUENCY_MHZ = 1150.0f;
static const float BZM_TUNING_HIGH_FREQUENCY_MHZ = 1200.0f;
static const float BZM_TUNING_HIGH_2_FREQUENCY_MHZ = 1425.0f;

const bzm_tps546_profile_t BZM_TPS546_BIRDS_PROFILE = {
    .phase = 0xff,
    .smbalert_mask = {
        0x0200, 0x1800, 0xe800, 0x0000, 0x0000, 0x0100, 0x4200,
    },
    .frequency_switch_khz = 325,
    .sync_config = 0x00,
    .stack_config = 0x0000,
    .interleave = 0x0010,
    .misc_options = 0x0000,
    .pin_detect_override = 0x0000,
    .compensation_config = {0x13, 0x11, 0x8c, 0x1d, 0x06},
    .power_stage_config = 0x70,
    .telemetry_config = {0x03, 0x03, 0x03, 0x03, 0x03, 0x00},
    .vout_command = BZM_TPS546_FIXED_VOUT_V,
    .vout_trim = 0x0000,
    .vout_max = BZM_TPS546_MAX_VOUT_V,
    .vout_margin_high = 1.1f,
    .vout_margin_low = 0.90f,
    .vout_transition_rate = 0xe010,
    .vout_scale_loop = 0.125f,
    .vout_min = BZM_TPS546_MIN_VOUT_V,
    .vin_on = 11.0f,
    .vin_off = 10.5f,
    .iout_cal_gain = 0xc880,
    .iout_cal_offset = 0xe000,
    .vout_ov_fault_limit = 1.25f,
    .vout_ov_fault_response = 0xbd,
    .vout_ov_warn_limit = 1.16f,
    .vout_uv_warn_limit = 0.90f,
    .vout_uv_fault_limit = 0.75f,
    .vout_uv_fault_response = 0xbe,
    .iout_oc_fault_limit = 55.0f,
    .iout_oc_fault_response = 0xc0,
    .iout_oc_warn_limit = 50.0f,
    .ot_fault_limit = 145,
    .ot_fault_response = 0xff,
    .ot_warn_limit = 105,
    .vin_ov_fault_limit = 14.0f,
    .vin_ov_fault_response = 0xb7,
    .vin_uv_warn_limit = 11.0f,
    .ton_delay = 0,
    .ton_rise = 3,
    .ton_max_fault_limit = 0,
    .ton_max_fault_response = 0x3b,
    .toff_delay = 0,
    .toff_fall = 0,
};

bool bzm_power_voltage_is_allowed(float volts)
{
    return isfinite(volts) &&
           (volts == 0.0f ||
            fabsf(volts - BZM_TPS546_FIXED_VOUT_V) <=
                BZM_TPS546_VOUT_TOLERANCE_V);
}

bool bzm_power_runtime_voltage_is_allowed(float volts)
{
    return isfinite(volts) &&
           volts >= BZM_TPS546_BIRDS_PROFILE.vout_min &&
           volts <= BZM_TPS546_BIRDS_PROFILE.vout_max;
}

bool bzm_power_resolve_user_voltage(uint16_t millivolts, float *volts)
{
    if (volts == NULL) return false;
    const float requested_v = (float)millivolts / 1000.0f;
    if (!bzm_power_runtime_voltage_is_allowed(requested_v)) return false;
    *volts = requested_v;
    return true;
}

bool bzm_power_frequency_target_voltage(float frequency_mhz,
                                        float *voltage_v)
{
    if (voltage_v == NULL ||
        !bzm_frequency_request_is_valid(frequency_mhz)) {
        return false;
    }

    float target_mv = BZM_TUNING_BASE_VOLTAGE_MV;
    if (frequency_mhz >= BZM_TUNING_HIGH_2_FREQUENCY_MHZ) {
        target_mv +=
            (frequency_mhz - BZM_TUNING_HIGH_2_FREQUENCY_MHZ) /
            BZM_TUNING_CALIBRATION_FREQUENCY_MHZ *
            BZM_TUNING_CALIBRATION_VOLTAGE_MV * 1.5f;
    } else if (frequency_mhz >= BZM_TUNING_HIGH_FREQUENCY_MHZ) {
        target_mv +=
            (frequency_mhz - BZM_TUNING_HIGH_FREQUENCY_MHZ) /
            BZM_TUNING_CALIBRATION_FREQUENCY_MHZ *
            BZM_TUNING_CALIBRATION_VOLTAGE_MV * 1.5f;
    } else if (frequency_mhz >= BZM_TUNING_BALANCED_FREQUENCY_MHZ) {
        target_mv +=
            (frequency_mhz - BZM_TUNING_BALANCED_FREQUENCY_MHZ) /
            BZM_TUNING_CALIBRATION_FREQUENCY_MHZ *
            BZM_TUNING_CALIBRATION_VOLTAGE_MV;
    } else if (frequency_mhz >= BZM_TUNING_LOW_FREQUENCY_MHZ) {
        target_mv +=
            (frequency_mhz - BZM_TUNING_LOW_FREQUENCY_MHZ) /
            BZM_TUNING_CALIBRATION_FREQUENCY_MHZ *
            BZM_TUNING_CALIBRATION_VOLTAGE_MV;
    } else {
        target_mv +=
            (frequency_mhz - BZM_TUNING_LOW_FREQUENCY_MHZ) /
            BZM_TUNING_CALIBRATION_FREQUENCY_MHZ *
            BZM_TUNING_CALIBRATION_VOLTAGE_MV / 2.0f;
    }

    /* The tuning profile uses a fixed 25 C ambient and a -50 mV offset. */
    const uint32_t target_mv_truncated = (uint32_t)target_mv;
    float resolved_v =
        (target_mv_truncated - BZM_TUNING_AMBIENT_OFFSET_MV) / 1000.0f;
    /*
     * Preserve the tuning voltage curve while respecting the BZM absolute rail
     * limit. Qualification will cap any PLL domain that cannot pass here.
     */
    resolved_v = fminf(resolved_v, BZM_TPS546_BIRDS_PROFILE.vout_max);
    if (!bzm_power_runtime_voltage_is_allowed(resolved_v)) return false;

    *voltage_v = resolved_v;
    return true;
}

bool bzm_power_tuning_next_voltage(float initial_voltage_v,
                                   float current_voltage_v,
                                   float *next_voltage_v)
{
    if (next_voltage_v == NULL ||
        !bzm_power_runtime_voltage_is_allowed(initial_voltage_v) ||
        !bzm_power_runtime_voltage_is_allowed(current_voltage_v)) {
        return false;
    }

    const float adaptive_limit_v = fminf(
        BZM_TPS546_BIRDS_PROFILE.vout_max,
        initial_voltage_v +
            BZM_TUNING_VOLTAGE_STEP_V * BZM_TUNING_MAX_VOLTAGE_STEPS);
    if (current_voltage_v >=
        adaptive_limit_v - BZM_TPS546_VOUT_READBACK_TOLERANCE_V) {
        return false;
    }

    const float candidate_v =
        fminf(adaptive_limit_v,
              current_voltage_v + BZM_TUNING_VOLTAGE_STEP_V);
    if (!bzm_power_runtime_voltage_is_allowed(candidate_v)) return false;

    *next_voltage_v = candidate_v;
    return true;
}

static void retain_first_error(esp_err_t candidate, esp_err_t *result)
{
    if (*result == ESP_OK && candidate != ESP_OK) *result = candidate;
}

static esp_err_t power_down(const bzm_power_ops_t *ops, void *context)
{
    esp_err_t result = ESP_OK;
    retain_first_error(ops->set_5v_enabled(context, false), &result);
    retain_first_error(ops->set_vout(context, 0.0f), &result);
    retain_first_error(ops->set_regulator_enabled(context, false), &result);
    return result;
}

static bool valid_ops(const bzm_power_ops_t *ops)
{
    return ops != NULL && ops->set_5v_enabled != NULL &&
           ops->set_regulator_enabled != NULL && ops->set_vout != NULL &&
           ops->validate_power != NULL && ops->delay_ms != NULL;
}

esp_err_t bzm_power_set_rail_enabled(const bzm_power_ops_t *ops,
                                     void *context, bool enabled)
{
    if (!valid_ops(ops)) return ESP_ERR_INVALID_ARG;
    if (!enabled) return power_down(ops, context);

    esp_err_t err = ops->set_5v_enabled(context, false);
    if (err == ESP_OK) {
        err = ops->set_regulator_enabled(context, true);
    }
    if (err == ESP_OK) {
        ops->delay_ms(context, 100);
        err = ops->set_vout(context, BZM_TPS546_BIRDS_PROFILE.vout_command);
    }
    if (err == ESP_OK) {
        ops->delay_ms(context, 100);
        err = ops->validate_power(
            context, BZM_TPS546_BIRDS_PROFILE.vout_command);
    }
    if (err != ESP_OK) {
        power_down(ops, context);
    }
    return err;
}

esp_err_t bzm_power_set_runtime_voltage(const bzm_power_ops_t *ops,
                                        void *context, float volts)
{
    if (!valid_ops(ops) ||
        !bzm_power_runtime_voltage_is_allowed(volts)) {
        return ESP_ERR_INVALID_ARG;
    }

    esp_err_t err = ops->set_vout(context, volts);
    if (err == ESP_OK) {
        ops->delay_ms(context, 100);
        err = ops->validate_power(context, volts);
    }
    if (err != ESP_OK) power_down(ops, context);
    return err;
}

esp_err_t bzm_power_set_enabled(const bzm_power_ops_t *ops, void *context,
                                bool enabled)
{
    if (!valid_ops(ops)) return ESP_ERR_INVALID_ARG;
    if (!enabled) return bzm_power_set_rail_enabled(ops, context, false);

    esp_err_t err = bzm_power_set_rail_enabled(ops, context, true);
    if (err != ESP_OK) return err; /* rail helper already rolled back */
    err = ops->set_5v_enabled(context, true);
    if (err != ESP_OK) power_down(ops, context);
    return err;
}
