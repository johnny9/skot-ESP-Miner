#ifndef BZM_DRIVER_H
#define BZM_DRIVER_H

#include <stdbool.h>
#include <stdint.h>

#include "bzm_result.h"
#include "bzm_bringup.h"
#include "bzm_dispatch_gate.h"
#include "bzm_transport.h"
#include "mining.h"

typedef struct GlobalState GlobalState;

/* The large Bonanza reactor and transport state is created only after board
 * detection selects BZM, and must reside in external RAM. */
bool BZM_driver_state_init(GlobalState *state);

int BZM_set_max_baud(void);
void BZM_submit_job(GlobalState *state, const asic_job_t *job);
bool BZM_clear_work(GlobalState * state);
double BZM_job_frequency_ms(GlobalState *state);
task_result * BZM_process_work(GlobalState * state);
float BZM_read_temperature(GlobalState * state);
bool BZM_hashrate_counter_snapshot(GlobalState *state,
                                   uint32_t *difficulty_one_counters,
                                   size_t counter_count);

/* Asynchronous transport failures are consumed by the board power owner. */
bool BZM_has_io_fault(void);
bool BZM_work_replacement_snapshot(uint32_t *generation, uint32_t *completed, bool *pending);

/* Thread-safe copies of the singleton transport's telemetry. */
bool BZM_get_telemetry_snapshot(bzm_telemetry_store_t * snapshot);
/* Ordered hardware startup operations. Mining dispatch stays closed until
 * all engines are active and result reporting is enabled. */
bzm_bringup_outcome_t BZM_initialize_transport(GlobalState * state, bzm_bringup_report_t * report);
bzm_bringup_outcome_t BZM_discover_chain(bzm_bringup_report_t * report);
bzm_bringup_outcome_t BZM_configure_sensors(const bzm_bringup_sensor_profile_t * profile,
                                         const bzm_bringup_telemetry_policy_t * telemetry_policy, bzm_bringup_report_t * report);
bzm_bringup_outcome_t BZM_configure_clocks(const bzm_bringup_pll_profile_t * profile,
                                        const bzm_bringup_telemetry_policy_t * telemetry_policy, bzm_bringup_report_t * report);
bzm_bringup_outcome_t BZM_step_frequency_domains(
    const float
        target_mhz[BZM_BRINGUP_ASIC_COUNT][BZM_BRINGUP_PLL_COUNT],
    bool allow_initial_jump, bzm_bringup_report_t *report,
    float *actual_mhz);
bzm_bringup_outcome_t BZM_activate_engines(const bzm_bringup_telemetry_policy_t * telemetry_policy,
                                               bzm_bringup_report_t * report);
bzm_bringup_outcome_t BZM_start_mining(GlobalState * state, const bzm_bringup_telemetry_policy_t * telemetry_policy,
                                         bzm_bringup_report_t * report);
bool BZM_get_state(bzm_bringup_state_t * state);
bool BZM_hold_reset(void);
/* The callback is evaluated before dispatch and before every engine write.
 * It must be non-blocking and must not call back into this driver. NULL is
 * fail-closed. */
void BZM_set_dispatch_authorizer(bzm_dispatch_authorizer_t authorize, void * context);
/* Evaluated before every startup bridge/UART operation, including while
 * startup holds the board mutex, to enforce cancellation and its deadline. */
void BZM_set_operation_authorizer(bzm_dispatch_authorizer_t authorize, void * context);
/* Pump the singleton parser for health monitoring; returns emitted frames. */
size_t BZM_poll(uint16_t timeout_ms);

#endif // BZM_DRIVER_H
