#include "board_io_hardware.h"

#include "display.h"
#include "power.h"
#include "thermal.h"
#include "vcore.h"

static const board_io_backend_t backend = {
    .display_init = display_init,
    .display_set_enabled = display_on,
    .power_get_output = Power_get_output,
    .power_get_input_voltage = Power_get_input_voltage,
    .power_get_vreg_temp = Power_get_vreg_temp,
    .vcore_init = VCORE_init,
    .vcore_set_voltage = VCORE_set_voltage,
    .vcore_get_voltage_mv = VCORE_get_voltage_mv,
    .vcore_check_fault = VCORE_check_fault,
    .vcore_get_fault_string = VCORE_get_fault_string,
    .thermal_init = Thermal_init,
    .fan_set_percent = Thermal_set_fan_percent,
    .fan_get_speed = Thermal_get_fan_speed,
    .fan_get_speed_2 = Thermal_get_fan2_speed,
    .thermal_get_chip_temp = Thermal_get_chip_temp,
    .thermal_get_chip_temp_2 = Thermal_get_chip_temp2,
};

const board_io_backend_t *board_io_hardware_backend(void)
{
    return &backend;
}
