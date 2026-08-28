#ifndef BOARD_IO_H
#define BOARD_IO_H

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

typedef struct GlobalState GlobalState;
typedef struct DeviceConfig DeviceConfig;

typedef struct {
    esp_err_t (*display_init)(GlobalState *state);
    esp_err_t (*display_set_enabled)(bool enabled);
    void (*power_get_output)(GlobalState *state, float *power, float *current);
    float (*power_get_input_voltage)(GlobalState *state);
    float (*power_get_vreg_temp)(GlobalState *state);
    esp_err_t (*vcore_init)(GlobalState *state);
    esp_err_t (*vcore_set_voltage)(GlobalState *state, float voltage);
    int16_t (*vcore_get_voltage_mv)(GlobalState *state);
    esp_err_t (*vcore_check_fault)(GlobalState *state);
    const char *(*vcore_get_fault_string)(GlobalState *state);
    esp_err_t (*thermal_init)(DeviceConfig *config);
    esp_err_t (*fan_set_percent)(DeviceConfig *config, float percent);
    uint16_t (*fan_get_speed)(DeviceConfig *config);
    uint16_t (*fan_get_speed_2)(DeviceConfig *config);
    float (*thermal_get_chip_temp)(GlobalState *state);
    float (*thermal_get_chip_temp_2)(GlobalState *state);
} board_io_backend_t;

void board_io_set_backend(const board_io_backend_t *backend);
void board_io_reset_backend(void);

esp_err_t board_io_display_init(GlobalState *state);
esp_err_t board_io_display_set_enabled(bool enabled);
void board_io_power_get_output(GlobalState *state, float *power, float *current);
float board_io_power_get_input_voltage(GlobalState *state);
float board_io_power_get_vreg_temp(GlobalState *state);
esp_err_t board_io_vcore_init(GlobalState *state);
esp_err_t board_io_vcore_set_voltage(GlobalState *state, float voltage);
int16_t board_io_vcore_get_voltage_mv(GlobalState *state);
esp_err_t board_io_vcore_check_fault(GlobalState *state);
const char *board_io_vcore_get_fault_string(GlobalState *state);
esp_err_t board_io_thermal_init(DeviceConfig *config);
esp_err_t board_io_fan_set_percent(DeviceConfig *config, float percent);
uint16_t board_io_fan_get_speed(DeviceConfig *config);
uint16_t board_io_fan_get_speed_2(DeviceConfig *config);
float board_io_thermal_get_chip_temp(GlobalState *state);
float board_io_thermal_get_chip_temp_2(GlobalState *state);

#endif
