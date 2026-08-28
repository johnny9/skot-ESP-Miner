#include "board_io_fake.h"

#include <pthread.h>
#include <string.h>

static pthread_mutex_t fake_lock = PTHREAD_MUTEX_INITIALIZER;
static board_io_fake_snapshot_t fake;

static esp_err_t fake_display_init(GlobalState *state)
{
    (void)state;
    pthread_mutex_lock(&fake_lock);
    ++fake.display_init_calls;
    esp_err_t result = fake.errors.display_init;
    pthread_mutex_unlock(&fake_lock);
    return result;
}

static esp_err_t fake_display_set_enabled(bool enabled)
{
    pthread_mutex_lock(&fake_lock);
    fake.display_enabled = enabled;
    esp_err_t result = fake.errors.display_set_enabled;
    pthread_mutex_unlock(&fake_lock);
    return result;
}

static void fake_power_get_output(GlobalState *state, float *power, float *current)
{
    (void)state;
    pthread_mutex_lock(&fake_lock);
    *power = fake.values.output_power;
    *current = fake.values.output_current;
    pthread_mutex_unlock(&fake_lock);
}

static float fake_power_get_input_voltage(GlobalState *state)
{
    (void)state;
    pthread_mutex_lock(&fake_lock);
    float result = fake.values.input_voltage;
    pthread_mutex_unlock(&fake_lock);
    return result;
}

static float fake_power_get_vreg_temp(GlobalState *state)
{
    (void)state;
    pthread_mutex_lock(&fake_lock);
    float result = fake.values.vreg_temp;
    pthread_mutex_unlock(&fake_lock);
    return result;
}

static esp_err_t fake_vcore_init(GlobalState *state)
{
    (void)state;
    pthread_mutex_lock(&fake_lock);
    ++fake.vcore_init_calls;
    esp_err_t result = fake.errors.vcore_init;
    pthread_mutex_unlock(&fake_lock);
    return result;
}

static esp_err_t fake_vcore_set_voltage(GlobalState *state, float voltage)
{
    (void)state;
    pthread_mutex_lock(&fake_lock);
    fake.requested_core_voltage = voltage;
    esp_err_t result = fake.errors.vcore_set_voltage;
    pthread_mutex_unlock(&fake_lock);
    return result;
}

static int16_t fake_vcore_get_voltage_mv(GlobalState *state)
{
    (void)state;
    pthread_mutex_lock(&fake_lock);
    int16_t result = fake.values.core_voltage_mv;
    pthread_mutex_unlock(&fake_lock);
    return result;
}

static esp_err_t fake_vcore_check_fault(GlobalState *state)
{
    (void)state;
    pthread_mutex_lock(&fake_lock);
    ++fake.fault_check_calls;
    esp_err_t result = fake.errors.vcore_check_fault;
    pthread_mutex_unlock(&fake_lock);
    return result;
}

static const char *fake_vcore_get_fault_string(GlobalState *state)
{
    (void)state;
    return "fake power fault";
}

static esp_err_t fake_thermal_init(DeviceConfig *config)
{
    (void)config;
    pthread_mutex_lock(&fake_lock);
    ++fake.thermal_init_calls;
    esp_err_t result = fake.errors.thermal_init;
    pthread_mutex_unlock(&fake_lock);
    return result;
}

static esp_err_t fake_fan_set_percent(DeviceConfig *config, float percent)
{
    (void)config;
    pthread_mutex_lock(&fake_lock);
    fake.requested_fan_percent = percent;
    esp_err_t result = fake.errors.fan_set_percent;
    pthread_mutex_unlock(&fake_lock);
    return result;
}

static uint16_t fake_fan_get_speed(DeviceConfig *config)
{
    (void)config;
    pthread_mutex_lock(&fake_lock);
    uint16_t result = fake.values.fan_speed;
    pthread_mutex_unlock(&fake_lock);
    return result;
}

static uint16_t fake_fan_get_speed_2(DeviceConfig *config)
{
    (void)config;
    pthread_mutex_lock(&fake_lock);
    uint16_t result = fake.values.fan_speed_2;
    pthread_mutex_unlock(&fake_lock);
    return result;
}

static float fake_thermal_get_chip_temp(GlobalState *state)
{
    (void)state;
    pthread_mutex_lock(&fake_lock);
    float result = fake.values.chip_temp;
    pthread_mutex_unlock(&fake_lock);
    return result;
}

static float fake_thermal_get_chip_temp_2(GlobalState *state)
{
    (void)state;
    pthread_mutex_lock(&fake_lock);
    float result = fake.values.chip_temp_2;
    pthread_mutex_unlock(&fake_lock);
    return result;
}

static const board_io_backend_t backend = {
    .display_init = fake_display_init,
    .display_set_enabled = fake_display_set_enabled,
    .power_get_output = fake_power_get_output,
    .power_get_input_voltage = fake_power_get_input_voltage,
    .power_get_vreg_temp = fake_power_get_vreg_temp,
    .vcore_init = fake_vcore_init,
    .vcore_set_voltage = fake_vcore_set_voltage,
    .vcore_get_voltage_mv = fake_vcore_get_voltage_mv,
    .vcore_check_fault = fake_vcore_check_fault,
    .vcore_get_fault_string = fake_vcore_get_fault_string,
    .thermal_init = fake_thermal_init,
    .fan_set_percent = fake_fan_set_percent,
    .fan_get_speed = fake_fan_get_speed,
    .fan_get_speed_2 = fake_fan_get_speed_2,
    .thermal_get_chip_temp = fake_thermal_get_chip_temp,
    .thermal_get_chip_temp_2 = fake_thermal_get_chip_temp_2,
};

const board_io_backend_t *board_io_fake_backend(void)
{
    return &backend;
}

void board_io_fake_reset(void)
{
    pthread_mutex_lock(&fake_lock);
    memset(&fake, 0, sizeof(fake));
    fake.errors.display_init = ESP_FAIL;
    fake.errors.display_set_enabled = ESP_FAIL;
    fake.errors.vcore_init = ESP_FAIL;
    fake.errors.vcore_set_voltage = ESP_FAIL;
    fake.errors.vcore_check_fault = ESP_FAIL;
    fake.errors.thermal_init = ESP_FAIL;
    fake.errors.fan_set_percent = ESP_FAIL;
    pthread_mutex_unlock(&fake_lock);
}

void board_io_fake_set_values(const board_io_fake_values_t *values)
{
    if (values == NULL) return;

    pthread_mutex_lock(&fake_lock);
    fake.values = *values;
    pthread_mutex_unlock(&fake_lock);
}

void board_io_fake_set_errors(const board_io_fake_errors_t *errors)
{
    if (errors == NULL) return;

    pthread_mutex_lock(&fake_lock);
    fake.errors = *errors;
    pthread_mutex_unlock(&fake_lock);
}

void board_io_fake_get_snapshot(board_io_fake_snapshot_t *snapshot)
{
    if (snapshot == NULL) return;

    pthread_mutex_lock(&fake_lock);
    *snapshot = fake;
    pthread_mutex_unlock(&fake_lock);
}
