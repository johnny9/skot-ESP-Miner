#ifndef BM1397_FIXTURE_BINDINGS_H
#define BM1397_FIXTURE_BINDINGS_H

#include <stdint.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#ifdef vTaskDelay
#undef vTaskDelay
#endif
#define vTaskDelay bm1397_fixture_delay
#define SERIAL_send bm1397_fixture_serial_send
#define SERIAL_set_baud bm1397_fixture_serial_set_baud
#define SERIAL_clear_buffer bm1397_fixture_serial_clear_buffer
#define receive_work bm1397_fixture_receive_work
#define count_asic_chips bm1397_fixture_count_chips
#define do_frequency_transition bm1397_fixture_frequency_transition

void bm1397_fixture_delay(TickType_t ticks);

#define BM1397_init fixture_BM1397_init
#define BM1397_send_work fixture_BM1397_send_work
#define BM1397_set_version_mask fixture_BM1397_set_version_mask
#define BM1397_set_max_baud fixture_BM1397_set_max_baud
#define BM1397_send_hash_frequency fixture_BM1397_send_hash_frequency
#define BM1397_process_work fixture_BM1397_process_work
#define BM1397_read_registers fixture_BM1397_read_registers

#endif
