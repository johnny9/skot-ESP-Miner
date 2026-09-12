#ifndef RESULT_FIXTURE_BINDINGS_H_
#define RESULT_FIXTURE_BINDINGS_H_

#include <inttypes.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#ifdef vTaskDelay
#undef vTaskDelay
#endif
#define vTaskDelay fixture_result_delay
#define ASIC_result_task fixture_ASIC_result_task
#define ASIC_process_work fixture_result_process_work
#define stratum_submit_share fixture_result_submit
#define self_test_record_nonce fixture_result_self_test
#define SYSTEM_notify_found_nonce fixture_result_notify
#define scoreboard_add fixture_result_score
#define hashrate_monitor_register_read fixture_result_register

void fixture_result_delay(TickType_t ticks);

#include "../../../main/tasks/asic_result_task.h"

#endif /* RESULT_FIXTURE_BINDINGS_H_ */
