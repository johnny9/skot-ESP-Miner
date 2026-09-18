#ifndef BZM_DRIVER_H
#define BZM_DRIVER_H

#include <stdbool.h>
#include <stdint.h>

#include "bzm_result.h"
#include "bzm_bringup.h"
#include "bzm_dispatch_gate.h"
#include "bzm_running_evidence.h"
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

/* Lock-free Stage-7 evidence counters. A baseline/current delta represents
 * exactly one RUNNING validation attempt. */
bool BZM_running_stats_snapshot(bzm_running_stats_t * stats);
bool BZM_work_replacement_snapshot(uint32_t *generation, uint32_t *completed, bool *pending);

/* Thread-safe copies of the singleton transport's receive diagnostics. */
bool BZM_get_telemetry_snapshot(bzm_telemetry_store_t * snapshot);
bool BZM_get_parser_stats(bzm_serial_parser_stats_t * stats);
/* Stage 6's proven parser boundary, including only explicitly accepted TDM
 * transition discards. Available after a successful balanced-ramp barrier. */
bool BZM_staged_get_parser_baseline(bzm_serial_parser_stats_t * stats);
/* Parser boundary proven by the clean Stage-4 TDM startup settling window. */
bool BZM_staged_get_sensor_parser_baseline(bzm_serial_parser_stats_t * stats);
/* Parser boundary proven by a quiet pre-dispatch Stage-7 settling window. */
bool BZM_staged_get_running_parser_baseline(bzm_serial_parser_stats_t * stats);

/*
 * Production staged entry points. These keep mining dispatch closed until
 * RUNNING is proven. BALANCED_RAMP is additionally compile-gated and uses a
 * reference-style sequential pair activation with no more than one engine of
 * transient bottom/top skew.
 */
bzm_bringup_outcome_t BZM_staged_initialize(GlobalState * state, bzm_bringup_report_t * report);
bzm_bringup_outcome_t BZM_staged_chain4(bzm_bringup_report_t * report);
bzm_bringup_outcome_t BZM_staged_sensors(const bzm_bringup_sensor_profile_t * profile,
                                         const bzm_bringup_telemetry_policy_t * telemetry_policy, bzm_bringup_report_t * report);
bzm_bringup_outcome_t BZM_staged_clocks(const bzm_bringup_pll_profile_t * profile,
                                        const bzm_bringup_telemetry_policy_t * telemetry_policy, bzm_bringup_report_t * report);
bzm_bringup_outcome_t BZM_staged_frequency_domains_step_live(
    const float
        target_mhz[BZM_BRINGUP_ASIC_COUNT][BZM_BRINGUP_PLL_COUNT],
    bool allow_initial_jump, bzm_bringup_report_t *report,
    float *actual_mhz);
bzm_bringup_outcome_t BZM_staged_balanced_ramp(const bzm_bringup_telemetry_policy_t * telemetry_policy,
                                               bzm_bringup_report_t * report);
bzm_bringup_outcome_t BZM_staged_running(GlobalState * state, const bzm_bringup_telemetry_policy_t * telemetry_policy,
                                         bzm_bringup_report_t * report);
bool BZM_staged_get_state(bzm_bringup_state_t * state);
bool BZM_staged_hold_reset(void);
/* The callback is evaluated before dispatch and before every engine write.
 * It must be non-blocking and must not call back into this driver. NULL is
 * fail-closed. */
void BZM_staged_set_dispatch_authorizer(bzm_dispatch_authorizer_t authorize, void * context);
/* Evaluated before every staged bridge/UART operation. The runtime uses it
 * to enforce the request's absolute powered-execution deadline even while
 * the synchronous validation call owns the supervisor mutex. */
void BZM_staged_set_operation_authorizer(bzm_dispatch_authorizer_t authorize, void * context);
/* Pump the singleton parser for health monitoring; returns emitted frames. */
size_t BZM_staged_poll(uint16_t timeout_ms);

#endif // BZM_DRIVER_H
