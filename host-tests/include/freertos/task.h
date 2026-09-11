#ifndef ESP_MINER_HOST_FREERTOS_TASK_H
#define ESP_MINER_HOST_FREERTOS_TASK_H

#include "FreeRTOS.h"

BaseType_t xTaskNotifyWait(
    uint32_t bits_to_clear_on_entry, unsigned long bits_to_clear_on_exit,
    uint32_t *notification_value, TickType_t ticks_to_wait);

/*
 * Declaration only: tests that execute task timing must provide a behavioral
 * scheduler adapter instead of silently using a no-op delay.
 */
void vTaskDelay(TickType_t ticks);

#endif
