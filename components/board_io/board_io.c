#include "board_io.h"

#include <stddef.h>

static const board_io_backend_t *selected_backend;

void board_io_set_backend(const board_io_backend_t *backend)
{
    selected_backend = backend;
}

void board_io_reset_backend(void)
{
    selected_backend = NULL;
}

esp_err_t board_io_display_init(GlobalState *state)
{
    return selected_backend->display_init(state);
}

esp_err_t board_io_display_set_enabled(bool enabled)
{
    return selected_backend->display_set_enabled(enabled);
}

void board_io_power_get_output(GlobalState *state, float *power, float *current)
{
    selected_backend->power_get_output(state, power, current);
}

float board_io_power_get_input_voltage(GlobalState *state)
{
    return selected_backend->power_get_input_voltage(state);
}

float board_io_power_get_vreg_temp(GlobalState *state)
{
    return selected_backend->power_get_vreg_temp(state);
}

esp_err_t board_io_vcore_init(GlobalState *state)
{
    return selected_backend->vcore_init(state);
}

esp_err_t board_io_vcore_set_voltage(GlobalState *state, float voltage)
{
    return selected_backend->vcore_set_voltage(state, voltage);
}

int16_t board_io_vcore_get_voltage_mv(GlobalState *state)
{
    return selected_backend->vcore_get_voltage_mv(state);
}

esp_err_t board_io_vcore_check_fault(GlobalState *state)
{
    return selected_backend->vcore_check_fault(state);
}

const char *board_io_vcore_get_fault_string(GlobalState *state)
{
    return selected_backend->vcore_get_fault_string(state);
}

esp_err_t board_io_thermal_init(DeviceConfig *config)
{
    return selected_backend->thermal_init(config);
}

esp_err_t board_io_fan_set_percent(DeviceConfig *config, float percent)
{
    return selected_backend->fan_set_percent(config, percent);
}

uint16_t board_io_fan_get_speed(DeviceConfig *config)
{
    return selected_backend->fan_get_speed(config);
}

uint16_t board_io_fan_get_speed_2(DeviceConfig *config)
{
    return selected_backend->fan_get_speed_2(config);
}

float board_io_thermal_get_chip_temp(GlobalState *state)
{
    return selected_backend->thermal_get_chip_temp(state);
}

float board_io_thermal_get_chip_temp_2(GlobalState *state)
{
    return selected_backend->thermal_get_chip_temp_2(state);
}
