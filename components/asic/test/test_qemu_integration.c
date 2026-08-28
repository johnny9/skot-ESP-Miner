#include "unity.h"

#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"

#include "asic.h"
#include "asic_backend.h"
#include "asic_simulator.h"
#include "board_io.h"
#include "board_io_fake.h"

#include <stdint.h>

typedef struct {
    GlobalState *state;
    DeviceConfig *config;
    bm_job *job;
    QueueHandle_t result_queue;
} integration_context_t;

static void simulated_device_task(void *argument)
{
    integration_context_t *context = argument;
    task_result result = {
        .job_id = 5,
        .nonce = 0x12345678,
        .rolled_version = 0x20002000,
        .timestamp_us = 987654,
    };

    board_io_display_init(context->state);
    board_io_vcore_init(context->state);
    board_io_thermal_init(context->config);
    board_io_display_set_enabled(true);
    board_io_vcore_set_voltage(context->state, 1.2f);
    board_io_fan_set_percent(context->config, 0.8f);

    ASIC_init(context->state);
    ASIC_set_version_mask(context->state, 0x1fffe000);
    ASIC_send_work(context->state, context->job);
    asic_simulator_queue_result(&result);
    vTaskDelete(NULL);
}

static void simulated_result_task(void *argument)
{
    integration_context_t *context = argument;

    for (;;) {
        task_result *result = ASIC_process_work(context->state);
        if (result != NULL) {
            xQueueSend(context->result_queue, result, 0);
            vTaskDelete(NULL);
        }
        vTaskDelay(1);
    }
}

TEST_CASE("QEMU runs simulated devices across FreeRTOS tasks", "[qemu-integration]")
{
    integration_context_t context = {
        .state = (GlobalState *)(uintptr_t)0x1234,
        .config = (DeviceConfig *)(uintptr_t)0x2345,
        .job = (bm_job *)(uintptr_t)0x3456,
        .result_queue = xQueueCreate(1, sizeof(task_result)),
    };
    TEST_ASSERT_NOT_NULL(context.result_queue);

    board_io_fake_values_t values = {
        .input_voltage = 5000.0f,
        .chip_temp = 55.0f,
        .fan_speed = 4200,
    };
    board_io_fake_errors_t no_errors = {0};
    board_io_fake_reset();
    board_io_fake_set_values(&values);
    board_io_fake_set_errors(&no_errors);
    board_io_set_backend(board_io_fake_backend());
    asic_simulator_reset();
    asic_simulator_configure(1, 115200, 500.0);
    ASIC_set_backend(asic_simulator_backend());

    TEST_ASSERT_EQUAL(pdPASS, xTaskCreate(simulated_result_task, "sim result", 4096, &context, 4, NULL));
    TEST_ASSERT_EQUAL(pdPASS, xTaskCreate(simulated_device_task, "sim device", 4096, &context, 5, NULL));

    task_result actual;
    TEST_ASSERT_EQUAL(pdTRUE, xQueueReceive(context.result_queue, &actual, pdMS_TO_TICKS(1000)));
    TEST_ASSERT_EQUAL_UINT8(5, actual.job_id);
    TEST_ASSERT_EQUAL_HEX32(0x12345678, actual.nonce);
    TEST_ASSERT_EQUAL_HEX32(0x20002000, actual.rolled_version);

    asic_simulator_snapshot_t asic_snapshot;
    asic_simulator_get_snapshot(&asic_snapshot);
    TEST_ASSERT_EQUAL_PTR(context.job, asic_snapshot.last_job);
    TEST_ASSERT_EQUAL_HEX32(0x1fffe000, asic_snapshot.version_mask);

    board_io_fake_snapshot_t board_snapshot;
    board_io_fake_get_snapshot(&board_snapshot);
    TEST_ASSERT_TRUE(board_snapshot.display_enabled);
    TEST_ASSERT_EQUAL_FLOAT(1.2f, board_snapshot.requested_core_voltage);
    TEST_ASSERT_EQUAL_FLOAT(0.8f, board_snapshot.requested_fan_percent);

    vTaskDelay(1);
    vQueueDelete(context.result_queue);
    ASIC_reset_backend();
    board_io_reset_backend();
}
