#ifndef ESP_MINER_HOST_FREERTOS_H
#define ESP_MINER_HOST_FREERTOS_H

#include <stdint.h>

typedef uint32_t TickType_t;
typedef int BaseType_t;

#define pdMS_TO_TICKS(milliseconds) ((TickType_t)(milliseconds))
#define pdTRUE 1
#define pdFALSE 0
#define portTICK_PERIOD_MS 1

#endif
