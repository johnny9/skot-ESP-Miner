#include "unity.h"

#include "board_io.h"
#include "board_io_fake.h"

#include <stdint.h>

TEST_CASE("Board I/O fake supplies measurements and records output", "[board_io]")
{
    GlobalState *state = (GlobalState *)(uintptr_t)0x1234;
    DeviceConfig *config = (DeviceConfig *)(uintptr_t)0x4321;
    board_io_fake_values_t values = {
        .input_voltage = 5000.0f,
        .output_power = 18.5f,
        .output_current = 4.25f,
        .vreg_temp = 62.0f,
        .core_voltage_mv = 1200,
        .chip_temp = 58.5f,
        .chip_temp_2 = 59.5f,
        .fan_speed = 4200,
        .fan_speed_2 = 4100,
    };
    board_io_fake_errors_t no_errors = {0};

    board_io_fake_reset();
    board_io_fake_set_values(&values);
    board_io_fake_set_errors(&no_errors);
    board_io_set_backend(board_io_fake_backend());

    TEST_ASSERT_EQUAL(ESP_OK, board_io_display_init(state));
    TEST_ASSERT_EQUAL(ESP_OK, board_io_display_set_enabled(true));
    TEST_ASSERT_EQUAL(ESP_OK, board_io_vcore_init(state));
    TEST_ASSERT_EQUAL(ESP_OK, board_io_vcore_set_voltage(state, 1.2f));
    TEST_ASSERT_EQUAL(ESP_OK, board_io_vcore_check_fault(state));
    TEST_ASSERT_EQUAL(ESP_OK, board_io_thermal_init(config));
    TEST_ASSERT_EQUAL(ESP_OK, board_io_fan_set_percent(config, 0.75f));

    float power;
    float current;
    board_io_power_get_output(state, &power, &current);
    TEST_ASSERT_EQUAL_FLOAT(values.output_power, power);
    TEST_ASSERT_EQUAL_FLOAT(values.output_current, current);
    TEST_ASSERT_EQUAL_FLOAT(values.input_voltage, board_io_power_get_input_voltage(state));
    TEST_ASSERT_EQUAL_FLOAT(values.vreg_temp, board_io_power_get_vreg_temp(state));
    TEST_ASSERT_EQUAL_INT16(values.core_voltage_mv, board_io_vcore_get_voltage_mv(state));
    TEST_ASSERT_EQUAL_STRING("fake power fault", board_io_vcore_get_fault_string(state));
    TEST_ASSERT_EQUAL_FLOAT(values.chip_temp, board_io_thermal_get_chip_temp(state));
    TEST_ASSERT_EQUAL_FLOAT(values.chip_temp_2, board_io_thermal_get_chip_temp_2(state));
    TEST_ASSERT_EQUAL_UINT16(values.fan_speed, board_io_fan_get_speed(config));
    TEST_ASSERT_EQUAL_UINT16(values.fan_speed_2, board_io_fan_get_speed_2(config));

    board_io_fake_snapshot_t snapshot;
    board_io_fake_get_snapshot(&snapshot);
    TEST_ASSERT_TRUE(snapshot.display_enabled);
    TEST_ASSERT_EQUAL_FLOAT(1.2f, snapshot.requested_core_voltage);
    TEST_ASSERT_EQUAL_FLOAT(0.75f, snapshot.requested_fan_percent);
    TEST_ASSERT_EQUAL_UINT32(1, snapshot.display_init_calls);
    TEST_ASSERT_EQUAL_UINT32(1, snapshot.vcore_init_calls);
    TEST_ASSERT_EQUAL_UINT32(1, snapshot.thermal_init_calls);
    TEST_ASSERT_EQUAL_UINT32(1, snapshot.fault_check_calls);

    board_io_reset_backend();
}

TEST_CASE("Board I/O fake defaults to safe failures", "[board_io]")
{
    board_io_fake_reset();
    board_io_set_backend(board_io_fake_backend());

    TEST_ASSERT_EQUAL(ESP_FAIL, board_io_display_init(NULL));
    TEST_ASSERT_EQUAL(ESP_FAIL, board_io_display_set_enabled(false));
    TEST_ASSERT_EQUAL(ESP_FAIL, board_io_vcore_init(NULL));
    TEST_ASSERT_EQUAL(ESP_FAIL, board_io_vcore_set_voltage(NULL, 0.0f));
    TEST_ASSERT_EQUAL(ESP_FAIL, board_io_vcore_check_fault(NULL));
    TEST_ASSERT_EQUAL(ESP_FAIL, board_io_thermal_init(NULL));
    TEST_ASSERT_EQUAL(ESP_FAIL, board_io_fan_set_percent(NULL, 1.0f));

    board_io_reset_backend();
}
