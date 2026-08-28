#ifndef BOARD_IO_FAKE_H
#define BOARD_IO_FAKE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "board_io.h"

typedef struct {
    float input_voltage;
    float output_power;
    float output_current;
    float vreg_temp;
    int16_t core_voltage_mv;
    float chip_temp;
    float chip_temp_2;
    uint16_t fan_speed;
    uint16_t fan_speed_2;
} board_io_fake_values_t;

typedef struct {
    esp_err_t display_init;
    esp_err_t display_set_enabled;
    esp_err_t vcore_init;
    esp_err_t vcore_set_voltage;
    esp_err_t vcore_check_fault;
    esp_err_t thermal_init;
    esp_err_t fan_set_percent;
} board_io_fake_errors_t;

typedef struct {
    board_io_fake_values_t values;
    board_io_fake_errors_t errors;
    bool display_enabled;
    float requested_core_voltage;
    float requested_fan_percent;
    size_t display_init_calls;
    size_t vcore_init_calls;
    size_t thermal_init_calls;
    size_t fault_check_calls;
} board_io_fake_snapshot_t;

const board_io_backend_t *board_io_fake_backend(void);
void board_io_fake_reset(void);
void board_io_fake_set_values(const board_io_fake_values_t *values);
void board_io_fake_set_errors(const board_io_fake_errors_t *errors);
void board_io_fake_get_snapshot(board_io_fake_snapshot_t *snapshot);

#endif
