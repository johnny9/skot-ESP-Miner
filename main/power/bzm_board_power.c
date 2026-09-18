#include "bzm_board_power.h"
#include "global_state.h"
#include "bzm_supervisor.h"
#include "bonanza_power_task.h"

#include <math.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>

#include "bonanza_tps546.h"
#include "bzm_driver.h"
#include "bzm_frequency.h"
#include "bzm_lease_guard.h"
#include "bzm_power.h"
#include "bzm_running_evidence.h"
#include "bzm_runtime_health.h"
#include "driver/gpio.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs_config.h"
#include "thermal.h"
#include "bonanza_vcore.h"

#define BZM_MONITOR_PERIOD_MS 100U
#define BZM_HEALTH_PERIOD_MS 500U
#define BZM_SAFE_OFF_TIMEOUT_MS 1500U
#define BZM_SAFE_OFF_SAMPLE_MS 25U
#define BZM_BOARD_WATCHDOG_MS 300000U
#define BZM_IO_TASK_PRIORITY 18U
#define BZM_FREQUENCY_TRANSITION_PROOF_TIMEOUT_MS 30000U

typedef struct
{
    pthread_mutex_t lock;
    GlobalState * global_state;
    bzm_supervisor_t supervisor;
    bzm_bridge_info_t bridge_info;
    bzm_bridge_safety_status_t bridge_status;
    bzm_bridge_rx_stats_t bridge_rx_stats;
    bool active;
    bool initialized;
    bool monitor_running;
    bool bridge_info_valid;
    bool bridge_status_valid;
    bool bridge_rx_stats_valid;
    bool mining_stack_ready;
    uint32_t replacement_generation;
    uint64_t replacement_started_ms;
    bool replacement_pending;
    atomic_bool pause_requested;
    atomic_bool dispatch_enabled;
    atomic_uint_fast64_t dispatch_deadline_ms;
    atomic_uint_fast64_t execution_deadline_ms;
    atomic_bool execution_cancelled;
    bool parser_baseline_valid;
    bzm_serial_parser_stats_t parser_baseline;
    bool parser_realign_valid;
    bzm_parser_realign_t parser_realign;
    bool health_valid;
    uint64_t health_sampled_at_ms;
    bzm_runtime_health_result_t health;
    bool tps_sample_valid;
    bzm_runtime_health_tps_sample_t tps_sample;
    bool parser_sample_valid;
    bzm_serial_parser_stats_t parser_sample;
    uint32_t parser_recovery_count;
    float board_temperature_c;
    bzm_ch2_confirmation_t ch2_confirmation;
    bzm_pll_lock_confirmation_t pll_lock_confirmation;
    bool running_evidence_requested;
    bool running_evidence_monitoring;
    uint64_t running_evidence_started_at_ms;
    bzm_running_stats_t running_evidence_baseline;
    bzm_running_evidence_lifecycle_t running_evidence_lifecycle;
    bzm_running_evidence_result_t running_evidence;
    bool running_evidence_frequency_transition;
    float frequency_target_mhz;
    float voltage_target_v;
    uint32_t frequency_target_generation;
    bool frequency_ramp_active;
    float rail_command_v;
    uint16_t fan_rpm;
} bzm_runtime_state_t;

static const char * TAG = "bzm_board_power";
static bool bridge_control_contract_compatible(
    const bzm_bridge_safety_status_t *status);
static bzm_runtime_state_t *RUNTIME_STATE;
#define RUNTIME (*RUNTIME_STATE)

static bool runtime_state_init(GlobalState *global_state)
{
    if (global_state == NULL ||
        global_state->DEVICE_CONFIG.family.asic.id != BZM ||
        !(global_state->DEVICE_CONFIG.family.id == BONANZA)) {
        return false;
    }
    if (RUNTIME_STATE != NULL) return true;

    bzm_runtime_state_t *allocated = heap_caps_calloc(
        1, sizeof(*allocated), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (allocated == NULL) {
        ESP_LOGE(TAG, "Unable to allocate Bonanza board state in PSRAM");
        return false;
    }
    if (pthread_mutex_init(&allocated->lock, NULL) != 0) {
        heap_caps_free(allocated);
        return false;
    }
    RUNTIME_STATE = allocated;
    ESP_LOGI(TAG, "Allocated %u bytes of Bonanza board state in PSRAM",
             (unsigned)sizeof(*allocated));
    return true;
}

static bzm_bringup_telemetry_policy_t telemetry_policy(void);

static bool runtime_is_holding_locked(void);
static bzm_runtime_health_result_t sample_runtime_health_locked(void);
static bool recoverable_overheat_fault(
    bzm_runtime_health_fault_t fault);

static bzm_running_evidence_config_t running_evidence_config(void)
{
    return (bzm_running_evidence_config_t){
        .required_chip_engine_writes = BZM_ENGINES_PER_ASIC * BZM_BRINGUP_ASIC_COUNT,
        .minimum_valid_results = CONFIG_BZM_1002_MIN_VALID_RESULTS,
        .allow_mapping_recovery = true,
        .maximum_mapping_rejections = CONFIG_BZM_1002_MAX_MAPPING_REJECTIONS,
        .maximum_local_rejections = CONFIG_BZM_1002_MAX_LOCAL_REJECTIONS,
        .proof_timeout_ms = CONFIG_BZM_1002_PROOF_TIMEOUT_SECONDS * 1000U,
        .recovery_timeout_ms = CONFIG_BZM_1002_RESULT_RECOVERY_TIMEOUT_MS,
    };
}

static void reset_running_evidence_locked(bool requested)
{
    RUNTIME.running_evidence_requested = requested;
    RUNTIME.running_evidence_monitoring = false;
    RUNTIME.running_evidence_started_at_ms = 0;
    RUNTIME.running_evidence_baseline = (bzm_running_stats_t){0};
    RUNTIME.running_evidence_frequency_transition = false;
    bzm_running_evidence_lifecycle_init(&RUNTIME.running_evidence_lifecycle);
    RUNTIME.running_evidence = (bzm_running_evidence_result_t){
        .status = BZM_RUNNING_EVIDENCE_PENDING,
        .fault = BZM_RUNNING_EVIDENCE_FAULT_NONE,
    };
    snprintf(RUNTIME.running_evidence.detail, sizeof(RUNTIME.running_evidence.detail), "%s",
             requested ? "waiting for mining proof" : "mining proof is not active");
}

static bzm_running_evidence_result_t evaluate_running_evidence_locked(uint64_t current_ms)
{
    if (!RUNTIME.running_evidence_monitoring)
        return RUNTIME.running_evidence;

    bzm_running_stats_t current = {0};
    if (!BZM_running_stats_snapshot(&current)) {
        RUNTIME.running_evidence = (bzm_running_evidence_result_t){
            .status = BZM_RUNNING_EVIDENCE_BAD,
            .fault = BZM_RUNNING_EVIDENCE_FAULT_INVALID_CONFIGURATION,
        };
        snprintf(RUNTIME.running_evidence.detail, sizeof(RUNTIME.running_evidence.detail),
                 "mining driver evidence is unavailable");
        return RUNTIME.running_evidence;
    }
    bzm_running_evidence_config_t config = running_evidence_config();
    if (RUNTIME.running_evidence_frequency_transition) {
        config.recovery_timeout_ms =
            BZM_FREQUENCY_TRANSITION_PROOF_TIMEOUT_MS;
    }
    RUNTIME.running_evidence =
        bzm_running_evidence_track(&RUNTIME.running_evidence_lifecycle,
                                   &RUNTIME.running_evidence_baseline,
                                   &current, &config,
                                   RUNTIME.running_evidence_started_at_ms,
                                   current_ms);
    if (RUNTIME.running_evidence_frequency_transition &&
        RUNTIME.running_evidence.status == BZM_RUNNING_EVIDENCE_GOOD &&
        RUNTIME.running_evidence.observed.dispatch_batches != 0 &&
        RUNTIME.running_evidence.observed.dispatched_chip_engines >=
            config.required_chip_engine_writes &&
        RUNTIME.running_evidence.observed.mapped_results >=
            config.minimum_valid_results &&
        RUNTIME.running_evidence.observed.locally_valid_results >=
            config.minimum_valid_results &&
        !RUNTIME.running_evidence.observed.mapping_recovery_pending &&
        !RUNTIME.running_evidence.observed.local_recovery_pending) {
        RUNTIME.running_evidence_frequency_transition = false;
        ESP_LOGI(TAG,
                 "BZM live frequency transition established a full "
                 "replacement dispatch and local nonce proof");
    }
    if (RUNTIME.running_evidence.status == BZM_RUNNING_EVIDENCE_GOOD && RUNTIME.parser_realign_valid &&
        RUNTIME.parser_realign.recovering) {
        snprintf(RUNTIME.running_evidence.detail, sizeof(RUNTIME.running_evidence.detail),
                 "proof retained; bounded parser realignment clean windows %u/%u",
                 (unsigned) RUNTIME.parser_realign.clean_windows, (unsigned) CONFIG_BZM_1002_PARSER_REALIGN_CLEAN_WINDOWS);
    }
    if (RUNTIME.running_evidence.status == BZM_RUNNING_EVIDENCE_GOOD) {
        snprintf(RUNTIME.supervisor.report.stages[BZM_STAGE_RUNNING].detail,
                 sizeof(RUNTIME.supervisor.report.stages[BZM_STAGE_RUNNING].detail), "RUNNING GOOD: %.140s",
                 RUNTIME.running_evidence.detail);
    } else if (RUNTIME.running_evidence.status == BZM_RUNNING_EVIDENCE_BAD) {
        RUNTIME.supervisor.report.stages[BZM_STAGE_RUNNING] =
            bzm_validation_result(BZM_CHECK_BAD, BZM_VALIDATION_CODE_STAGE_FAILED, RUNTIME.running_evidence.detail);
    }
    return RUNTIME.running_evidence;
}

static uint64_t now_ms(void)
{
    return (uint64_t) (esp_timer_get_time() / 1000);
}

static void close_dispatch_locked(void)
{
    atomic_store_explicit(&RUNTIME.dispatch_enabled, false, memory_order_release);
    atomic_store_explicit(&RUNTIME.dispatch_deadline_ms, 0, memory_order_release);
}

static bool runtime_dispatch_authorizer(void * context)
{
    bzm_runtime_state_t * runtime = context;
    if (runtime == NULL || BONANZA_POWER_MANAGEMENT_stop_requested() ||
        (runtime->global_state != NULL && runtime->global_state->SYSTEM_MODULE.pools_unavailable) ||
        atomic_load_explicit(&runtime->pause_requested,
                             memory_order_acquire) ||
        !atomic_load_explicit(&runtime->dispatch_enabled,
                              memory_order_acquire)) {
        return false;
    }
    uint64_t deadline = atomic_load_explicit(&runtime->dispatch_deadline_ms, memory_order_acquire);
    return deadline != 0 && now_ms() < deadline;
}

static bool runtime_execution_authorizer(void * context)
{
    bzm_runtime_state_t * runtime = context;
    if (runtime == NULL) return false;
    if (BONANZA_POWER_MANAGEMENT_stop_requested() ||
        (runtime->global_state != NULL && runtime->global_state->SYSTEM_MODULE.pools_unavailable)) {
        atomic_store_explicit(&runtime->execution_cancelled, true, memory_order_release);
        return false;
    }
    uint64_t deadline = atomic_load_explicit(&runtime->execution_deadline_ms, memory_order_acquire);
    return bzm_lease_guard_deadline_allows(deadline, now_ms());
}

static void sync_dispatch_locked(void)
{
    uint64_t current_ms = now_ms();
    if (RUNTIME.health.status == BZM_RUNTIME_HEALTH_BAD ||
        !bzm_supervisor_dispatch_allowed(&RUNTIME.supervisor, current_ms)) {
        close_dispatch_locked();
        return;
    }

    /* Publish the deadline before opening the gate. The driver re-evaluates
     * this callback before dispatch and before every engine write. */
    atomic_store_explicit(&RUNTIME.dispatch_deadline_ms, RUNTIME.supervisor.lease_deadline_ms, memory_order_release);
    atomic_store_explicit(&RUNTIME.dispatch_enabled, true, memory_order_release);
}

static bool start_production_mining_locked(void)
{
    if (!RUNTIME.initialized || !RUNTIME.mining_stack_ready ||
        RUNTIME.supervisor.owner != BZM_SUPERVISOR_OWNER_NONE ||
        RUNTIME.supervisor.fault_latched) {
        return false;
    }

    close_dispatch_locked();
    reset_running_evidence_locked(true);
    bzm_ch2_confirmation_init(&RUNTIME.ch2_confirmation);
    bzm_pll_lock_confirmation_init(&RUNTIME.pll_lock_confirmation);

    const uint32_t lease_ms = RUNTIME.supervisor.config.maximum_lease_ms;
    const uint64_t started_ms = now_ms();
    uint64_t execution_deadline_ms = 0;
    if (!bzm_lease_guard_make_deadline(started_ms, lease_ms,
                                       &execution_deadline_ms)) {
        return false;
    }
    atomic_store_explicit(&RUNTIME.execution_deadline_ms,
                          execution_deadline_ms, memory_order_release);
    /* Power management is the production authority for this locked
     * profile. The fresh arm is internal and cannot be supplied remotely. */
    bool completed = bzm_supervisor_request_validation(
        &RUNTIME.supervisor, BZM_STAGE_RUNNING, true, true, lease_ms,
        started_ms);
    atomic_store_explicit(&RUNTIME.execution_deadline_ms, 0,
                          memory_order_release);

    if (completed && runtime_is_holding_locked()) {
        bzm_runtime_health_result_t health = sample_runtime_health_locked();
        if (health.status == BZM_RUNTIME_HEALTH_BAD) {
            close_dispatch_locked();
            (void)bzm_supervisor_latch_fault(
                &RUNTIME.supervisor, (uint32_t)health.fault, health.detail);
            completed = false;
        }
    }
    if (!completed ||
        RUNTIME.supervisor.owner != BZM_SUPERVISOR_OWNER_MINING) {
        if (!RUNTIME.supervisor.fault_latched &&
            !atomic_load_explicit(&RUNTIME.execution_cancelled, memory_order_acquire)) {
            (void)bzm_supervisor_latch_fault(
                &RUNTIME.supervisor, 0x1006,
                "production mining task stack could not start");
        }
        sync_dispatch_locked();
        return false;
    }

    RUNTIME.running_evidence_started_at_ms = now_ms();
    RUNTIME.running_evidence_monitoring = true;
    RUNTIME.global_state->ASIC_initalized = true;
    (void)evaluate_running_evidence_locked(
        RUNTIME.running_evidence_started_at_ms);
    sync_dispatch_locked();
    return true;
}

static bool runtime_is_holding_locked(void)
{
    return RUNTIME.supervisor.owner == BZM_SUPERVISOR_OWNER_VALIDATION || RUNTIME.supervisor.owner == BZM_SUPERVISOR_OWNER_MINING;
}

static bzm_runtime_health_result_t sample_runtime_health_locked(void)
{
    bzm_runtime_health_input_t input = {
        .reached_stage = RUNTIME.supervisor.report.reached_stage,
        .holding = runtime_is_holding_locked(),
        .bridge_status_available = RUNTIME.bridge_status_valid,
        .bridge_status = RUNTIME.bridge_status,
        .require_independent_kill =
            RUNTIME.supervisor.config.production_mode && !RUNTIME.supervisor.config.board_managed_safety &&
            RUNTIME.supervisor.report.reached_stage >= BZM_STAGE_POWER_RAIL,
        .fan_min_rpm = CONFIG_BZM_1002_FAN_MIN_RPM,
        .require_fan_full =
            RUNTIME.supervisor.report.reached_stage < BZM_STAGE_RUNNING,
        .tps_bounds =
            {
                .vin_min_v = BZM_TPS546_BIRDS_PROFILE.vin_off,
                .vin_max_v = BZM_TPS546_BIRDS_PROFILE.vin_ov_fault_limit,
                .vout_command_v = RUNTIME.rail_command_v,
                .vout_command_tolerance_v = BZM_TPS546_VOUT_READBACK_TOLERANCE_V,
                .vout_min_v =
                    RUNTIME.rail_command_v -
                    BZM_TPS546_VOUT_OPERATING_TOLERANCE_V,
                .vout_max_v =
                    RUNTIME.rail_command_v +
                    BZM_TPS546_VOUT_OPERATING_TOLERANCE_V,
                .iout_min_a = -1.0f,
                .iout_max_a = BZM_TPS546_BIRDS_PROFILE.iout_oc_warn_limit,
                .temperature_min_c = -40.0f,
                .temperature_max_c =
                    (float)BZM_TPS546_BIRDS_PROFILE.ot_warn_limit,
            },
        .telemetry_bounds = telemetry_policy().bounds,
        .telemetry_now_us = (uint64_t) esp_timer_get_time(),
        .telemetry_max_age_us = telemetry_policy().max_age_us,
        .defer_ch2_bounds = true,
        .defer_clock_locks = true,
        .parser_stats_available = RUNTIME.parser_baseline_valid,
        .parser_baseline = RUNTIME.parser_baseline,
    };

    if (input.holding) {
        bzm_bridge_safety_status_t status;
        input.bridge_status_available = BZM_bridge_safety_heartbeat(&status) == ESP_OK && status.valid;
        if (input.bridge_status_available) {
            input.bridge_status = status;
            RUNTIME.bridge_status = status;
            RUNTIME.bridge_status_valid = true;
            if (RUNTIME.global_state != NULL) {
                RUNTIME.global_state->POWER_MANAGEMENT_MODULE.fan_perc =
                    status.fan_percent;
            }
        } else {
            RUNTIME.bridge_status_valid = false;
        }
        input.fan_tach_available = BZM_bridge_get_fan_rpm(&input.fan_rpm) == ESP_OK;
        if (input.fan_tach_available) {
            RUNTIME.fan_rpm = input.fan_rpm;
            if (RUNTIME.global_state != NULL) {
                RUNTIME.global_state->POWER_MANAGEMENT_MODULE.fan_rpm =
                    input.fan_rpm;
            }
        }
        bzm_bridge_rx_stats_t rx_stats;
        RUNTIME.bridge_rx_stats_valid =
            BZM_bridge_get_rx_stats(&rx_stats) == ESP_OK && rx_stats.valid;
        if (RUNTIME.bridge_rx_stats_valid)
            RUNTIME.bridge_rx_stats = rx_stats;
    }

    if (input.holding && input.reached_stage >= BZM_STAGE_POWER_RAIL) {
        BONANZA_TPS546_StatusSnapshot power = {0};
        bool pgood = false;
        input.tps.available = BONANZA_VCORE_bzm_snapshot(&power, &pgood) == ESP_OK;
        if (input.tps.available) {
            input.tps.pgood = pgood;
            input.tps.operation = power.operation;
            input.tps.status_word = power.status_word;
            input.tps.vout_command_v = power.vout_command;
            input.tps.vout_command_matches_expected =
                isfinite(power.vout_command) &&
                fabsf(power.vout_command - RUNTIME.rail_command_v) <=
                    BZM_TPS546_VOUT_READBACK_TOLERANCE_V;
            input.tps.vin_v = power.read_vin;
            input.tps.vout_v = power.read_vout;
            input.tps.iout_a = power.read_iout;
            input.tps.temperature_c = power.read_temp1;
            if (RUNTIME.global_state != NULL) {
                PowerManagementModule *management =
                    &RUNTIME.global_state->POWER_MANAGEMENT_MODULE;
                management->voltage = power.read_vin * 1000.0f;
                management->core_voltage = power.read_vout * 1000.0f;
                management->current = power.read_iout * 1000.0f;
                management->power =
                    power.read_vout * power.read_iout +
                    RUNTIME.global_state->DEVICE_CONFIG.family.power_offset;
                management->vr_temp = power.read_temp1;
            }
        }
        RUNTIME.tps_sample = input.tps;
        RUNTIME.tps_sample_valid = input.tps.available;
    }

    if (input.holding && input.reached_stage >= BZM_STAGE_CHAIN_4) {
        (void) BZM_staged_poll(25);
        input.parser_stats_available = RUNTIME.parser_baseline_valid && BZM_get_parser_stats(&input.parser_current);
    }

    bzm_parser_realign_result_t parser_realign_result = BZM_PARSER_REALIGN_CLEAN;
    uint32_t parser_realign_discarded = 0;
    uint32_t parser_realign_unexpected_registers = 0;
    if (input.holding && input.reached_stage == BZM_STAGE_RUNNING && input.parser_stats_available && RUNTIME.parser_realign_valid) {
        parser_realign_result = bzm_parser_realign_observe(
            &RUNTIME.parser_realign, &input.parser_current, CONFIG_BZM_1002_PARSER_REALIGN_MAX_DISCARDS,
            CONFIG_BZM_1002_PARSER_REALIGN_CLEAN_WINDOWS, CONFIG_BZM_1002_PARSER_REALIGN_MAX_WINDOWS,
            CONFIG_BZM_1002_PARSER_REALIGN_MAX_EVENTS);
        if (RUNTIME.parser_realign.recovering || parser_realign_result == BZM_PARSER_REALIGN_RECOVERED) {
            parser_realign_discarded = input.parser_current.discarded_bytes - RUNTIME.parser_realign.burst_discard_baseline;
            parser_realign_unexpected_registers = input.parser_current.unexpected_register_headers -
                                                  RUNTIME.parser_realign.burst_unexpected_register_baseline;
        }
        if (parser_realign_result == BZM_PARSER_REALIGN_PENDING || parser_realign_result == BZM_PARSER_REALIGN_RECOVERED) {
            /* The realignment state machine proved that every rejected
             * register-header increment accompanied discarded bytes and that
             * every other parser counter remained exact. Suppress only this
             * bounded realignment episode while independent checks continue. */
            input.parser_baseline.discarded_bytes = input.parser_current.discarded_bytes;
            input.parser_baseline.unexpected_register_headers = input.parser_current.unexpected_register_headers;
        }
        if (parser_realign_result == BZM_PARSER_REALIGN_RECOVERED) {
            RUNTIME.parser_baseline = RUNTIME.parser_realign.accepted;
            RUNTIME.parser_recovery_count++;
            ESP_LOGW(TAG, "Mining parser realigned after %lu discarded bytes and %lu rejected register headers; valid frame resumed and %u clean windows passed",
                     (unsigned long) parser_realign_discarded, (unsigned long) parser_realign_unexpected_registers,
                     (unsigned) CONFIG_BZM_1002_PARSER_REALIGN_CLEAN_WINDOWS);
        }
    }

    if (input.holding && input.reached_stage >= BZM_STAGE_SENSORS) {
        input.telemetry_available = BZM_get_telemetry_snapshot(&input.telemetry);
        /* Timestamp the completed snapshot, not the start of the copy. The
         * transport can publish a fresh sample while this task is waiting for
         * its mutex; taking `now` first made that valid sample appear a few
         * hundred microseconds in the future and falsely latched safe-off. */
        input.telemetry_now_us = (uint64_t) esp_timer_get_time();
        if (input.telemetry_available) {
            const bool temperature_valid = bzm_telemetry_max_temperature(
                &input.telemetry, input.telemetry_now_us,
                input.telemetry_max_age_us, &RUNTIME.board_temperature_c);
            if (temperature_valid && RUNTIME.global_state != NULL) {
                RUNTIME.global_state->POWER_MANAGEMENT_MODULE.chip_temp_avg =
                    RUNTIME.board_temperature_c;
                RUNTIME.global_state->POWER_MANAGEMENT_MODULE.chip_temp2_avg =
                    -1.0f;
            }
        }
    }

    RUNTIME.parser_sample = input.parser_current;
    RUNTIME.parser_sample_valid = input.parser_stats_available;

    RUNTIME.health = bzm_runtime_health_evaluate(&input);
    if (RUNTIME.health.status == BZM_RUNTIME_HEALTH_GOOD && parser_realign_result == BZM_PARSER_REALIGN_PENDING) {
        snprintf(RUNTIME.health.detail, sizeof(RUNTIME.health.detail),
                 "Mining parser realignment pending: discarded=%lu/%u rejectedHeaders=%lu bursts=%u/%u cleanWindows=%u/%u windows=%u/%u",
                 (unsigned long) parser_realign_discarded, (unsigned) CONFIG_BZM_1002_PARSER_REALIGN_MAX_DISCARDS,
                 (unsigned long) parser_realign_unexpected_registers,
                 (unsigned) RUNTIME.parser_realign.episode_bursts,
                 (unsigned) CONFIG_BZM_1002_PARSER_REALIGN_MAX_EVENTS,
                 (unsigned) RUNTIME.parser_realign.clean_windows, (unsigned) CONFIG_BZM_1002_PARSER_REALIGN_CLEAN_WINDOWS,
                 (unsigned) RUNTIME.parser_realign.observed_windows, (unsigned) CONFIG_BZM_1002_PARSER_REALIGN_MAX_WINDOWS);
    } else if (RUNTIME.health.status == BZM_RUNTIME_HEALTH_GOOD && parser_realign_result == BZM_PARSER_REALIGN_RECOVERED) {
        snprintf(RUNTIME.health.detail, sizeof(RUNTIME.health.detail),
                 "Mining parser realigned: discarded=%lu rejectedHeaders=%lu valid frame resumed cleanWindows=%u",
                 (unsigned long) parser_realign_discarded, (unsigned long) parser_realign_unexpected_registers,
                 (unsigned) RUNTIME.parser_realign.clean_windows);
    }
    if (RUNTIME.health.status == BZM_RUNTIME_HEALTH_GOOD && input.holding && input.reached_stage >= BZM_STAGE_CLOCKS &&
        input.telemetry_available) {
        uint8_t culprit_asic_id = 0;
        uint8_t observed_samples = 0;
        bzm_ch2_confirmation_result_t confirmation = bzm_pll_lock_confirmation_observe(
            &RUNTIME.pll_lock_confirmation, &input.telemetry, input.telemetry_now_us, input.telemetry_max_age_us,
            CONFIG_BZM_1002_PLL_LOCK_CONFIRM_SAMPLES, &culprit_asic_id, &observed_samples);
        if (confirmation == BZM_CH2_CONFIRMATION_CONTINUOUS || confirmation == BZM_CH2_CONFIRMATION_INVALID) {
            RUNTIME.health.status = BZM_RUNTIME_HEALTH_BAD;
            RUNTIME.health.fault = BZM_RUNTIME_HEALTH_FAULT_TELEMETRY_CLOCK_UNLOCKED;
            if (confirmation == BZM_CH2_CONFIRMATION_CONTINUOUS) {
                snprintf(RUNTIME.health.detail, sizeof(RUNTIME.health.detail),
                         "ASIC 0x%02x combined PLL lock clear continuously for %u/%u fresh samples",
                         (unsigned) culprit_asic_id, (unsigned) observed_samples,
                         (unsigned) CONFIG_BZM_1002_PLL_LOCK_CONFIRM_SAMPLES);
            } else {
                snprintf(RUNTIME.health.detail, sizeof(RUNTIME.health.detail),
                         "combined PLL lock confirmation input is invalid");
            }
        } else if (observed_samples != 0) {
            RUNTIME.health.fault = BZM_RUNTIME_HEALTH_FAULT_NONE;
            snprintf(RUNTIME.health.detail, sizeof(RUNTIME.health.detail),
                     confirmation == BZM_CH2_CONFIRMATION_NO_NEW_SAMPLE
                         ? "ASIC 0x%02x combined PLL lock clear pending a fresh sample at %u/%u"
                         : "ASIC 0x%02x combined PLL lock clear pending confirmation at %u/%u",
                     (unsigned) culprit_asic_id, (unsigned) observed_samples,
                     (unsigned) CONFIG_BZM_1002_PLL_LOCK_CONFIRM_SAMPLES);
        }
    }
    if (RUNTIME.health.status == BZM_RUNTIME_HEALTH_GOOD && input.holding && input.reached_stage >= BZM_STAGE_SENSORS &&
        input.telemetry_available) {
        uint8_t culprit_asic_id = 0;
        uint8_t observed_samples = 0;
        bzm_ch2_confirmation_result_t confirmation =
            bzm_ch2_confirmation_observe(&RUNTIME.ch2_confirmation, &input.telemetry, &input.telemetry_bounds,
                                         CONFIG_BZM_1002_CH2_CONFIRM_SAMPLES, &culprit_asic_id, &observed_samples);
        if (confirmation == BZM_CH2_CONFIRMATION_CONTINUOUS || confirmation == BZM_CH2_CONFIRMATION_INVALID) {
            RUNTIME.health.status = BZM_RUNTIME_HEALTH_BAD;
            RUNTIME.health.fault = BZM_RUNTIME_HEALTH_FAULT_TELEMETRY_BOUNDS;
            if (confirmation == BZM_CH2_CONFIRMATION_CONTINUOUS) {
                const bzm_telemetry_sample_t * sample = bzm_telemetry_store_get(&input.telemetry, culprit_asic_id);
                snprintf(RUNTIME.health.detail, sizeof(RUNTIME.health.detail),
                         "ASIC 0x%02x CH2 excursion continuous for %u/%u fresh samples: %.1f mV limit=+/-%.1f mV",
                         (unsigned) culprit_asic_id, (unsigned) observed_samples, (unsigned) CONFIG_BZM_1002_CH2_CONFIRM_SAMPLES,
                         sample != NULL ? sample->ch2_mv : NAN, input.telemetry_bounds.ch2_abs_max_mv);
            } else {
                snprintf(RUNTIME.health.detail, sizeof(RUNTIME.health.detail), "CH2 confirmation input is invalid");
            }
        } else if (observed_samples != 0) {
            RUNTIME.health.fault = BZM_RUNTIME_HEALTH_FAULT_NONE;
            snprintf(RUNTIME.health.detail, sizeof(RUNTIME.health.detail),
                     confirmation == BZM_CH2_CONFIRMATION_NO_NEW_SAMPLE
                         ? "ASIC 0x%02x CH2 excursion pending a fresh sample at %u/%u"
                         : "ASIC 0x%02x CH2 excursion pending confirmation at %u/%u",
                     (unsigned) culprit_asic_id, (unsigned) observed_samples, (unsigned) CONFIG_BZM_1002_CH2_CONFIRM_SAMPLES);
        }
    }
    RUNTIME.health_valid = true;
    RUNTIME.health_sampled_at_ms = now_ms();
    return RUNTIME.health;
}

static bool bridge_status_runtime_good(const bzm_bridge_safety_status_t * status)
{
    return status != NULL && status->valid && status->fault == BZM_BRIDGE_SAFETY_FAULT_NONE && !status->trip_input_asserted &&
           (status->runtime_verdict == BZM_BRIDGE_SAFETY_RUNTIME_GOOD_SAFE_OFF ||
            status->runtime_verdict == BZM_BRIDGE_SAFETY_RUNTIME_GOOD_CONTROLLED);
}

static bool bridge_control_contract_compatible(
    const bzm_bridge_safety_status_t *status)
{
    const uint16_t required = BZM_BRIDGE_SAFETY_CAP_5V_CONTROL |
                              BZM_BRIDGE_SAFETY_CAP_ASIC_RESET_CONTROL |
                              BZM_BRIDGE_SAFETY_CAP_FAN_FORCE_FULL |
                              BZM_BRIDGE_SAFETY_CAP_TRIP_INPUT_SAMPLED |
                              BZM_BRIDGE_SAFETY_CAP_FAN_CONTROLLED_SPEED;
    return bridge_status_runtime_good(status) &&
           status->stage == BZM_BRIDGE_SAFETY_STAGE_TRIP_LATCH &&
           (status->capabilities & required) == required;
}

static bool bridge_has_independent_kill(const bzm_bridge_safety_status_t * status)
{
    const uint16_t required = BZM_BRIDGE_SAFETY_CAP_CORE_POWER_CUTOFF | BZM_BRIDGE_SAFETY_CAP_FAN_TACH_INTERLOCK |
                              BZM_BRIDGE_SAFETY_CAP_INDEPENDENT_TRIP_MONITOR;
    return bridge_status_runtime_good(status) && status->stage == BZM_BRIDGE_SAFETY_STAGE_TRIP_LATCH &&
           status->production_verdict == BZM_BRIDGE_SAFETY_PRODUCTION_GOOD && (status->capabilities & required) == required &&
           (status->evidence & required) == required;
}

static void refresh_bridge_evidence_locked(void)
{
    RUNTIME.bridge_info_valid =
        BZM_bridge_get_info(&RUNTIME.bridge_info) == ESP_OK &&
        bzm_bridge_info_supports_raw_rx(&RUNTIME.bridge_info);
    RUNTIME.bridge_status_valid =
        RUNTIME.bridge_info_valid && BZM_bridge_get_safety_status(&RUNTIME.bridge_status) == ESP_OK && RUNTIME.bridge_status.valid;
    RUNTIME.bridge_rx_stats_valid =
        RUNTIME.bridge_info_valid &&
        BZM_bridge_get_rx_stats(&RUNTIME.bridge_rx_stats) == ESP_OK &&
        RUNTIME.bridge_rx_stats.valid;
    RUNTIME.supervisor.config.independent_kill_available =
        RUNTIME.bridge_status_valid && bridge_has_independent_kill(&RUNTIME.bridge_status);
}

static bzm_stage_result_t runtime_force_safe_off(void * context)
{
    GlobalState * state = context;
    bool commands_ok = state != NULL;
    bzm_bridge_safety_status_t status;

    close_dispatch_locked();
    RUNTIME.running_evidence_monitoring = false;
    RUNTIME.parser_baseline_valid = false;
    RUNTIME.parser_realign_valid = false;

    if (state == NULL) {
        return bzm_validation_result(BZM_CHECK_BAD, BZM_VALIDATION_CODE_SAFE_OFF_FAILED,
                                     "global state is unavailable; shutdown cannot be verified");
    }

    state->ASIC_initalized = false;
    (void) BZM_staged_hold_reset();
    BZM_staged_set_dispatch_authorizer(runtime_dispatch_authorizer, &RUNTIME);

    /* Disarm is best-effort for recovery from legacy unversioned firmware. */
    (void) BZM_bridge_disarm_safety(&status);
    commands_ok = BZM_bridge_set_asic_reset(false) == ESP_OK && commands_ok;
    commands_ok = BZM_bridge_set_5v_enabled(false) == ESP_OK && commands_ok;
    commands_ok = Thermal_set_fan_percent(&state->DEVICE_CONFIG, 1.0f) == ESP_OK && commands_ok;
    commands_ok = BONANZA_VCORE_bzm_set_rail_enabled(state, false) == ESP_OK && commands_ok;

    BONANZA_TPS546_StatusSnapshot power = {0};
    bool pgood = true;
    bool electrical_safe = false;
    for (uint32_t waited = 0; waited <= BZM_SAFE_OFF_TIMEOUT_MS; waited += BZM_SAFE_OFF_SAMPLE_MS) {
        if (BONANZA_VCORE_bzm_snapshot(&power, &pgood) == ESP_OK && !pgood && (power.operation & OPERATION_ON) == 0 &&
            power.read_vout * 1000.0f <= (float) CONFIG_BZM_1002_SAFE_OFF_VCORE_MV) {
            electrical_safe = true;
            break;
        }
        vTaskDelay(pdMS_TO_TICKS(BZM_SAFE_OFF_SAMPLE_MS));
    }

    RUNTIME.bridge_status_valid = BZM_bridge_get_safety_status(&RUNTIME.bridge_status) == ESP_OK && RUNTIME.bridge_status.valid;
    if (electrical_safe && RUNTIME.bridge_status_valid &&
        RUNTIME.bridge_status.fault != BZM_BRIDGE_SAFETY_FAULT_NONE &&
        bzm_bridge_safety_status_allows_fault_clear(&RUNTIME.bridge_status)) {
        /* The RP2040 intentionally retains lease/trip faults across an ESP
         * reboot. Clear that latch only after this boot has independently
         * forced safe bridge outputs and proved the TPS rail off and
         * discharged, then evaluate the returned status from scratch. */
        bzm_bridge_safety_status_t cleared = {0};
        esp_err_t clear_err = BZM_bridge_clear_safety_fault(&cleared);
        commands_ok = clear_err == ESP_OK && commands_ok;
        RUNTIME.bridge_status = cleared;
        RUNTIME.bridge_status_valid = clear_err == ESP_OK && cleared.valid;
        if (RUNTIME.bridge_status_valid) {
            ESP_LOGW(TAG, "Cleared retained bridge safety fault after verified electrical safe-off");
        } else {
            ESP_LOGE(TAG, "Retained bridge safety fault clear failed: %s", esp_err_to_name(clear_err));
        }
    }
    const uint16_t required_safe_evidence =
        BZM_BRIDGE_SAFETY_EVIDENCE_OUTPUTS_SAFE | BZM_BRIDGE_SAFETY_EVIDENCE_TRIP_CLEAR | BZM_BRIDGE_SAFETY_EVIDENCE_FAULT_CLEAR;
    bool bridge_safe = RUNTIME.bridge_status_valid && RUNTIME.bridge_status.state == BZM_BRIDGE_SAFETY_STATE_SAFE_OFF &&
                       RUNTIME.bridge_status.fault == BZM_BRIDGE_SAFETY_FAULT_NONE &&
                       RUNTIME.bridge_status.runtime_verdict == BZM_BRIDGE_SAFETY_RUNTIME_GOOD_SAFE_OFF &&
                       RUNTIME.bridge_status.lease_remaining_ms == 0 && !RUNTIME.bridge_status.five_volt_enabled &&
                       RUNTIME.bridge_status.asic_reset_asserted && RUNTIME.bridge_status.fan_full &&
                       RUNTIME.bridge_status.fan_percent == 100 && !RUNTIME.bridge_status.trip_input_asserted &&
                       (RUNTIME.bridge_status.evidence & required_safe_evidence) == required_safe_evidence;

    if (!commands_ok || !electrical_safe || !bridge_safe) {
        char detail[BZM_VALIDATION_DETAIL_LENGTH];
        snprintf(detail, sizeof(detail),
                 "safe-off failed: commands=%s pgood=%s vout=%.3fV "
                 "operation=0x%02x bridge=%s",
                 commands_ok ? "ok" : "bad", pgood ? "high" : "low", power.read_vout, power.operation,
                 bridge_safe ? "safe" : (RUNTIME.bridge_status_valid ? "unsafe" : "unavailable"));
        ESP_LOGE(TAG, "%s", detail);
        return bzm_validation_result(BZM_CHECK_BAD, BZM_VALIDATION_CODE_SAFE_OFF_FAILED, detail);
    }

    return bzm_validation_result(BZM_CHECK_GOOD, BZM_VALIDATION_CODE_STAGE_OK,
                                 "reset asserted, 5 V and TPS off, fan full, PGOOD low, "
                                 "VCORE discharged, bridge safety readback coherent");
}

static bzm_stage_result_t run_controls(GlobalState * state)
{
    bzm_bridge_safety_status_t status;
    if (BZM_bridge_get_info(&RUNTIME.bridge_info) != ESP_OK ||
        !bzm_bridge_info_supports_raw_rx(&RUNTIME.bridge_info)) {
        RUNTIME.bridge_info_valid = false;
        return bzm_validation_result(BZM_CHECK_BLOCKED, BZM_VALIDATION_CODE_NOT_IMPLEMENTED,
                                     "bridge protocol 1.0 raw RX and receive stats are required");
    }
    RUNTIME.bridge_info_valid = true;
    if (BZM_bridge_get_rx_stats(&RUNTIME.bridge_rx_stats) != ESP_OK ||
        !RUNTIME.bridge_rx_stats.valid) {
        RUNTIME.bridge_rx_stats_valid = false;
        return bzm_validation_result(BZM_CHECK_BAD, BZM_VALIDATION_CODE_STAGE_FAILED,
                                     "bridge receive stats are unavailable or malformed");
    }
    RUNTIME.bridge_rx_stats_valid = true;

    if (BZM_bridge_get_safety_status(&status) != ESP_OK || !status.valid ||
        status.state != BZM_BRIDGE_SAFETY_STATE_SAFE_OFF ||
        !bridge_control_contract_compatible(&status)) {
        return bzm_validation_result(BZM_CHECK_BAD, BZM_VALIDATION_CODE_STAGE_FAILED,
                                     "bridge is unsafe, incompatible, or lacks required lease/trip control paths");
    }
    if (BZM_bridge_arm_safety(&status) != ESP_OK || status.state != BZM_BRIDGE_SAFETY_STATE_CONTROLLED ||
        status.lease_remaining_ms == 0 || BZM_bridge_safety_heartbeat(&status) != ESP_OK ||
        status.state != BZM_BRIDGE_SAFETY_STATE_CONTROLLED || status.lease_remaining_ms == 0 ||
        !bridge_status_runtime_good(&status)) {
        return bzm_validation_result(BZM_CHECK_BAD, BZM_VALIDATION_CODE_STAGE_FAILED, "bridge arm/heartbeat lease proof failed");
    }
    if (Thermal_set_fan_percent(&state->DEVICE_CONFIG, 1.0f) != ESP_OK || BZM_bridge_get_fan_rpm(&RUNTIME.fan_rpm) != ESP_OK ||
        RUNTIME.fan_rpm < CONFIG_BZM_1002_FAN_MIN_RPM) {
        (void) BZM_bridge_disarm_safety(&status);
        return bzm_validation_result(BZM_CHECK_BAD, BZM_VALIDATION_CODE_STAGE_FAILED,
                                     "fan full-speed command or fresh tach threshold failed");
    }
    if (BZM_bridge_disarm_safety(&status) != ESP_OK || !status.valid || status.state != BZM_BRIDGE_SAFETY_STATE_SAFE_OFF ||
        !status.asic_reset_asserted || status.five_volt_enabled || !status.fan_full) {
        return bzm_validation_result(BZM_CHECK_BAD, BZM_VALIDATION_CODE_STAGE_FAILED,
                                     "bridge did not return to coherent safe outputs after lease proof");
    }
    RUNTIME.bridge_status = status;
    RUNTIME.bridge_status_valid = true;
    return bzm_validation_result(BZM_CHECK_GOOD, BZM_VALIDATION_CODE_STAGE_OK,
                                 "protocol 1.0, safety status, lease heartbeat, trip-clear state and fan tach are GOOD");
}

static bzm_stage_result_t run_power_rail(GlobalState * state)
{
    bzm_bridge_safety_status_t status;
    if (BZM_bridge_arm_safety(&status) != ESP_OK || !status.valid || status.state != BZM_BRIDGE_SAFETY_STATE_CONTROLLED ||
        status.lease_remaining_ms == 0 || status.five_volt_enabled || !status.asic_reset_asserted || !status.fan_full ||
        !bridge_status_runtime_good(&status)) {
        return bzm_validation_result(BZM_CHECK_BAD, BZM_VALIDATION_CODE_STAGE_FAILED,
                                     "bridge lease could not be armed with reset asserted, 5 V off and fan full");
    }
    if (BONANZA_VCORE_bzm_set_rail_enabled(state, true) != ESP_OK) {
        return bzm_validation_result(BZM_CHECK_BAD, BZM_VALIDATION_CODE_STAGE_FAILED,
                                     "TPS rail enable or live power validation failed");
    }

    char profile_detail[128];
    if (BONANZA_TPS546_verify_active_config(profile_detail, sizeof(profile_detail)) != ESP_OK) {
        char detail[BZM_VALIDATION_DETAIL_LENGTH];
        snprintf(detail, sizeof(detail), "TPS profile readback mismatch: %s", profile_detail);
        return bzm_validation_result(BZM_CHECK_BAD, BZM_VALIDATION_CODE_STAGE_FAILED, detail);
    }

    BONANZA_TPS546_StatusSnapshot power;
    bool pgood = false;
    if (BONANZA_VCORE_bzm_snapshot(&power, &pgood) != ESP_OK || !pgood || (power.operation & OPERATION_ON) == 0 ||
        !power.vout_command_matches_active_config || !isfinite(power.vout_command) ||
        fabsf(power.vout_command - BZM_TPS546_FIXED_VOUT_V) > BZM_TPS546_VOUT_READBACK_TOLERANCE_V ||
        !isfinite(power.read_vin) || !isfinite(power.read_vout) || !isfinite(power.read_iout) ||
        power.read_vin < BZM_TPS546_BIRDS_PROFILE.vin_off || power.read_vout < 2.65f || power.read_vout > 2.95f ||
        power.read_iout < -1.0f || power.read_iout > BZM_TPS546_BIRDS_PROFILE.iout_oc_warn_limit ||
        power.read_temp1 > BZM_TPS546_BIRDS_PROFILE.ot_warn_limit || power.status_word != 0) {
        char detail[BZM_VALIDATION_DETAIL_LENGTH];
        snprintf(detail, sizeof(detail),
                 "TPS stage-2 bad: PGOOD=%u OP=0x%02x STATUS=0x%04x CMD=%.3fV RAW=0x%04x EXACT=%u VIN=%.2fV VOUT=%.3fV IOUT=%.2fA TEMP=%dC",
                 (unsigned) pgood, (unsigned) power.operation, (unsigned) power.status_word,
                 power.vout_command, (unsigned) power.vout_command_raw,
                 (unsigned) power.vout_command_matches_active_config, power.read_vin,
                 power.read_vout, power.read_iout, power.read_temp1);
        return bzm_validation_result(BZM_CHECK_BAD, BZM_VALIDATION_CODE_STAGE_FAILED, detail);
    }
    if (BZM_bridge_get_safety_status(&status) != ESP_OK || !status.valid || status.state != BZM_BRIDGE_SAFETY_STATE_CONTROLLED ||
        status.five_volt_enabled || !status.asic_reset_asserted || !status.fan_full || !bridge_status_runtime_good(&status)) {
        return bzm_validation_result(BZM_CHECK_BAD, BZM_VALIDATION_CODE_STAGE_FAILED,
                                     "bridge outputs changed unexpectedly while the TPS rail was validated");
    }
    RUNTIME.bridge_status = status;
    RUNTIME.bridge_status_valid = true;
    return bzm_validation_result(
        BZM_CHECK_GOOD, BZM_VALIDATION_CODE_STAGE_OK,
        "TPS command is locked at 2.8 V with PGOOD and fresh telemetry; reset remains asserted and bridge 5 V remains off");
}

static bzm_bringup_telemetry_policy_t telemetry_policy(void)
{
    return (bzm_bringup_telemetry_policy_t){
        .bounds =
            {
                .temperature_min_c = (float) CONFIG_BZM_1002_TEMP_MIN_C,
                .temperature_max_c = (float) CONFIG_BZM_1002_TEMP_MAX_C,
                .ch0_min_mv = (float) CONFIG_BZM_1002_STACK_MV_MIN,
                .ch0_max_mv = (float) CONFIG_BZM_1002_STACK_MV_MAX,
                .ch1_min_mv = (float) CONFIG_BZM_1002_STACK_MV_MIN,
                .ch1_max_mv = (float) CONFIG_BZM_1002_STACK_MV_MAX,
                .ch2_abs_max_mv = (float) CONFIG_BZM_1002_INTERSTACK_DIFF_ABS_MAX_MV,
                .max_stack_spread_mv = (float) CONFIG_BZM_1002_STACK_MAX_SPREAD_MV,
            },
        .max_age_us = (uint64_t) CONFIG_BZM_1002_TELEMETRY_MAX_AGE_MS * 1000U,
        .ch2_confirm_samples = CONFIG_BZM_1002_CH2_CONFIRM_SAMPLES,
    };
}

static bzm_stage_result_t bringup_stage_result(const char * stage_name, bzm_bringup_outcome_t outcome,
                                               const bzm_bringup_report_t * report)
{
    char detail[BZM_VALIDATION_DETAIL_LENGTH];
    snprintf(detail, sizeof(detail),
             "%s %s: reason=%s asic=0x%02x pll=%u reg=0x%02x "
             "expected=%lu actual=%lu completed=%u",
             stage_name, bzm_bringup_outcome_name(outcome),
             report != NULL ? bzm_bringup_reason_name(report->reason) : "missing_report", report != NULL ? report->asic_id : 0,
             report != NULL ? report->pll_index : 0, report != NULL ? report->register_offset : 0,
             (unsigned long) (report != NULL ? report->expected : 0), (unsigned long) (report != NULL ? report->actual : 0),
             report != NULL ? report->completed_items : 0);
    if (outcome == BZM_BRINGUP_GOOD) {
        return bzm_validation_result(BZM_CHECK_GOOD, BZM_VALIDATION_CODE_STAGE_OK, detail);
    }
    if (outcome == BZM_BRINGUP_BLOCKED) {
        bzm_validation_code_t code = report != NULL && report->reason == BZM_BRINGUP_REASON_BALANCED_PAIR_UNAVAILABLE
                                         ? BZM_VALIDATION_CODE_NOT_IMPLEMENTED
                                         : BZM_VALIDATION_CODE_PREREQUISITE_FAILED;
        return bzm_validation_result(BZM_CHECK_BLOCKED, code, detail);
    }
    return bzm_validation_result(BZM_CHECK_BAD, BZM_VALIDATION_CODE_STAGE_FAILED, detail);
}

static bzm_stage_result_t run_chain4(GlobalState * state)
{
    bzm_bridge_safety_status_t status;
    if (BZM_bridge_set_asic_reset(false) != ESP_OK || BZM_bridge_set_5v_enabled(true) != ESP_OK ||
        BZM_bridge_get_safety_status(&status) != ESP_OK || !status.valid || status.state != BZM_BRIDGE_SAFETY_STATE_CONTROLLED ||
        !status.five_volt_enabled || !status.asic_reset_asserted || !status.fan_full || !bridge_status_runtime_good(&status)) {
        return bzm_validation_result(BZM_CHECK_BAD, BZM_VALIDATION_CODE_STAGE_FAILED,
                                     "chain power/reset precondition failed: expected leased 5 V on, reset asserted and fan full");
    }

    bzm_bringup_report_t report;
    bzm_bringup_outcome_t outcome = BZM_staged_initialize(state, &report);
    if (outcome == BZM_BRINGUP_GOOD) {
        /* BZM_staged_initialize reconstructs the transport and closes its
         * gate. Reattach only the lock-free runtime predicate while the
         * predicate itself remains false. */
        BZM_staged_set_dispatch_authorizer(runtime_dispatch_authorizer, &RUNTIME);
        RUNTIME.parser_baseline_valid = BZM_get_parser_stats(&RUNTIME.parser_baseline);
        if (!RUNTIME.parser_baseline_valid) {
            outcome = BZM_BRINGUP_BAD;
            report = (bzm_bringup_report_t){
                .outcome = BZM_BRINGUP_BAD,
                .reason = BZM_BRINGUP_REASON_IO,
            };
        } else {
            outcome = BZM_staged_chain4(&report);
        }
    }
    if (outcome == BZM_BRINGUP_GOOD &&
        (BZM_bridge_get_safety_status(&status) != ESP_OK || !status.valid || status.state != BZM_BRIDGE_SAFETY_STATE_CONTROLLED ||
         !status.five_volt_enabled || status.asic_reset_asserted || !status.fan_full || !bridge_status_runtime_good(&status))) {
        return bzm_validation_result(BZM_CHECK_BAD, BZM_VALIDATION_CODE_STAGE_FAILED,
                                     "four-chip probe passed but bridge power/reset readback is incoherent");
    }
    RUNTIME.bridge_status = status;
    RUNTIME.bridge_status_valid = status.valid;
    return bringup_stage_result("CHAIN_4", outcome, &report);
}

static bzm_stage_result_t run_sensors(void)
{
    bzm_bringup_sensor_profile_t profile;
    bzm_bringup_reference_sensor_profile(&profile);
    bzm_bringup_telemetry_policy_t policy = telemetry_policy();
    bzm_bringup_report_t report;
    bzm_bringup_outcome_t outcome = BZM_staged_sensors(&profile, &policy, &report);
    if (outcome == BZM_BRINGUP_GOOD) {
        /* Telemetry startup begins the four TDM transmitters together and proves a
         * full clean parser interval after any bounded activation residue.
         * Runtime monitoring begins at that accepted boundary. */
        RUNTIME.parser_baseline_valid = BZM_staged_get_sensor_parser_baseline(&RUNTIME.parser_baseline);
        if (!RUNTIME.parser_baseline_valid) {
            return bzm_validation_result(BZM_CHECK_BAD, BZM_VALIDATION_CODE_STAGE_FAILED,
                                         "telemetry startup parser baseline is unavailable");
        }
    }
    return bringup_stage_result("SENSORS", outcome, &report);
}

static bzm_stage_result_t run_clocks(void)
{
    bzm_bringup_pll_profile_t profile;
    bzm_bringup_pll_800_profile(&profile);
    bzm_bringup_telemetry_policy_t policy = telemetry_policy();
    bzm_bringup_report_t report;
    bzm_bringup_outcome_t outcome = BZM_staged_clocks(&profile, &policy, &report);
    if (outcome == BZM_BRINGUP_GOOD && RUNTIME.global_state != NULL) {
        RUNTIME.global_state->POWER_MANAGEMENT_MODULE.actual_frequency =
            BZM_FREQUENCY_POWER_ON_MHZ;
    }
    return bringup_stage_result("CLOCKS", outcome, &report);
}

static bzm_stage_result_t run_balanced_ramp(void)
{
    bzm_bringup_telemetry_policy_t policy = telemetry_policy();
    bzm_bringup_report_t report;
    bzm_bringup_outcome_t outcome = BZM_staged_balanced_ramp(&policy, &report);
    if (outcome == BZM_BRINGUP_GOOD) {
        /* Engine activation deliberately pauses/resumes TDM and proves those transition
         * discards separately from every clean engine window. Start runtime
         * parser monitoring at that accepted boundary so proven transition
         * traffic is not misclassified as a live fault. */
        RUNTIME.parser_baseline_valid = BZM_staged_get_parser_baseline(&RUNTIME.parser_baseline);
        if (!RUNTIME.parser_baseline_valid) {
            return bzm_validation_result(BZM_CHECK_BAD, BZM_VALIDATION_CODE_STAGE_FAILED,
                                         "engine activation parser baseline is unavailable");
        }
    }
    return bringup_stage_result("BALANCED_RAMP", outcome, &report);
}

static bzm_stage_result_t run_running(GlobalState * state)
{
    if (!RUNTIME.mining_stack_ready) {
        return bzm_validation_result(BZM_CHECK_BLOCKED, BZM_VALIDATION_CODE_PREREQUISITE_FAILED,
                                     "network mining queue is not initialized yet");
    }
    bzm_bringup_telemetry_policy_t policy = telemetry_policy();
    bzm_bringup_report_t report;
    bzm_bringup_outcome_t outcome = BZM_staged_running(state, &policy, &report);
    if (outcome == BZM_BRINGUP_GOOD) {
        RUNTIME.parser_baseline_valid = BZM_staged_get_running_parser_baseline(&RUNTIME.parser_baseline);
        if (!RUNTIME.parser_baseline_valid) {
            return bzm_validation_result(BZM_CHECK_BAD, BZM_VALIDATION_CODE_STAGE_FAILED,
                                         "pre-mining quiet parser baseline is unavailable");
        }
        bzm_parser_realign_init(&RUNTIME.parser_realign, &RUNTIME.parser_baseline);
        RUNTIME.parser_realign_valid = RUNTIME.parser_realign.initialized;
        if (!RUNTIME.parser_realign_valid) {
            return bzm_validation_result(BZM_CHECK_BAD, BZM_VALIDATION_CODE_STAGE_FAILED,
                                         "mining parser realignment baseline is unavailable");
        }
    }
    if (outcome == BZM_BRINGUP_GOOD && !BZM_running_stats_snapshot(&RUNTIME.running_evidence_baseline)) {
        return bzm_validation_result(BZM_CHECK_BAD, BZM_VALIDATION_CODE_STAGE_FAILED,
                                     "mining driver evidence baseline is unavailable");
    }
    return bringup_stage_result("RUNNING", outcome, &report);
}

static bzm_stage_result_t runtime_run_stage(void * context, bzm_validation_stage_t stage)
{
    GlobalState * state = context;
    if (stage >= BZM_STAGE_POWER_RAIL && !runtime_execution_authorizer(&RUNTIME)) {
        bool cancelled = atomic_load_explicit(&RUNTIME.execution_cancelled, memory_order_acquire);
        return bzm_validation_result(cancelled ? BZM_CHECK_BLOCKED : BZM_CHECK_BAD,
            cancelled ? BZM_VALIDATION_CODE_PREREQUISITE_FAILED : BZM_VALIDATION_CODE_STAGE_FAILED,
            cancelled ? "startup cancelled before step entry" : "startup watchdog expired before step entry");
    }
    bzm_stage_result_t result;
    switch (stage) {
    case BZM_STAGE_OFF_SAFE:
        return runtime_force_safe_off(context);
    case BZM_STAGE_CONTROLS:
        result = run_controls(state);
        break;
    case BZM_STAGE_POWER_RAIL:
        result = run_power_rail(state);
        break;
    case BZM_STAGE_CHAIN_4:
        result = run_chain4(state);
        break;
    case BZM_STAGE_SENSORS:
        result = run_sensors();
        break;
    case BZM_STAGE_CLOCKS:
        result = run_clocks();
        break;
    case BZM_STAGE_BALANCED_RAMP:
        result = run_balanced_ramp();
        break;
    case BZM_STAGE_RUNNING:
        result = run_running(state);
        break;
    default:
        return bzm_validation_result(BZM_CHECK_BLOCKED, BZM_VALIDATION_CODE_INVALID_CONFIGURATION,
                                     "unknown Bonanza startup step");
    }
    if (stage >= BZM_STAGE_POWER_RAIL && !runtime_execution_authorizer(&RUNTIME)) {
        bool cancelled = atomic_load_explicit(&RUNTIME.execution_cancelled, memory_order_acquire);
        return bzm_validation_result(cancelled ? BZM_CHECK_BLOCKED : BZM_CHECK_BAD,
            cancelled ? BZM_VALIDATION_CODE_PREREQUISITE_FAILED : BZM_VALIDATION_CODE_STAGE_FAILED,
            cancelled ? "startup cancelled during step" : "startup watchdog expired during step");
    }
    return result;
}

static float expected_bzm_hashrate_ghs(const GlobalState *state,
                                       float frequency_mhz)
{
    if (state == NULL || !isfinite(frequency_mhz)) return 0.0f;
    return frequency_mhz * state->DEVICE_CONFIG.family.asic.core_count *
           state->DEVICE_CONFIG.family.asic_count * 4.0f / 3.0f / 1000.0f;
}

static bool configured_tuning_target(
    bzm_frequency_target_t *frequency_target, float *voltage_target_v)
{
    if (frequency_target == NULL || voltage_target_v == NULL) return false;

    const float requested_mhz =
        (RUNTIME.global_state->SELF_TEST_MODULE.is_active ? 800.0f : nvs_config_get_float(NVS_CONFIG_ASIC_FREQUENCY));
    const uint16_t requested_voltage_mv =
        (RUNTIME.global_state->SELF_TEST_MODULE.is_active ? 2800 : nvs_config_get_u16(NVS_CONFIG_ASIC_VOLTAGE));
    return bzm_frequency_request_is_valid(requested_mhz) &&
           bzm_frequency_resolve_target(requested_mhz, frequency_target) &&
           bzm_power_resolve_user_voltage(requested_voltage_mv,
                                          voltage_target_v);
}

static bool tuning_snapshot(bonanza_power_target_t target, float *target_mhz,
                                    float *target_voltage_v,
                                    uint32_t *generation)
{
    pthread_mutex_lock(&RUNTIME.lock);
    bzm_frequency_target_t frequency_target;
    float requested_voltage_v = 0.0f;
    const bool target_valid = bzm_frequency_request_is_valid(target.frequency_mhz) &&
        bzm_frequency_resolve_target(target.frequency_mhz, &frequency_target) &&
        bzm_power_resolve_user_voltage(target.voltage_mv, &requested_voltage_v);
    const bool target_changed =
        target_valid &&
        (fabsf(RUNTIME.frequency_target_mhz -
               frequency_target.actual_mhz) >= 0.001f ||
         fabsf(RUNTIME.voltage_target_v - requested_voltage_v) >= 0.001f);
    if (target_changed) {
        RUNTIME.frequency_target_mhz = frequency_target.actual_mhz;
        RUNTIME.voltage_target_v = requested_voltage_v;
        ++RUNTIME.frequency_target_generation;
        RUNTIME.frequency_ramp_active = true;
        if (RUNTIME.global_state != NULL) {
            RUNTIME.global_state->POWER_MANAGEMENT_MODULE.frequency_value =
                frequency_target.actual_mhz;
            RUNTIME.global_state->POWER_MANAGEMENT_MODULE.expected_hashrate =
                expected_bzm_hashrate_ghs(
                    RUNTIME.global_state, frequency_target.actual_mhz);
        }
        ESP_LOGI(TAG,
                 "BZM live mining target changed to %.3f MHz, %.3f V "
                 "(generation %lu)",
                 frequency_target.actual_mhz, requested_voltage_v,
                 (unsigned long)RUNTIME.frequency_target_generation);
    }

    const uint64_t current_ms = now_ms();
    /* Stage 7's baseline proof is collected at the known-good 800 MHz
     * startup point. Do not rewrite PLLs or the rail while that proof is in
     * flight: doing so resets the engine rotation underneath the evidence
     * window and can turn a healthy chain into a false startup timeout.
     * Once GOOD, live tuning continues while mining without a reboot. */
    const bool evidence_ready =
        RUNTIME.running_evidence.status == BZM_RUNNING_EVIDENCE_GOOD;
    const bool ready =
        target_valid && RUNTIME.initialized && RUNTIME.global_state != NULL &&
        !atomic_load_explicit(&RUNTIME.pause_requested,
                              memory_order_acquire) &&
        RUNTIME.supervisor.owner == BZM_SUPERVISOR_OWNER_MINING &&
        RUNTIME.supervisor.report.state == BZM_VALIDATION_HOLDING &&
        !RUNTIME.supervisor.fault_latched && evidence_ready &&
        bzm_supervisor_dispatch_allowed(&RUNTIME.supervisor, current_ms);
    if (target_mhz != NULL) *target_mhz = RUNTIME.frequency_target_mhz;
    if (target_voltage_v != NULL) {
        *target_voltage_v = RUNTIME.voltage_target_v;
    }
    if (generation != NULL) {
        *generation = RUNTIME.frequency_target_generation;
    }
    if (!ready && !target_valid) RUNTIME.frequency_ramp_active = false;
    pthread_mutex_unlock(&RUNTIME.lock);
    return ready;
}

static void tuning_set_active(bool active, uint32_t generation)
{
    pthread_mutex_lock(&RUNTIME.lock);
    if (RUNTIME.frequency_target_generation == generation) {
        RUNTIME.frequency_ramp_active = active;
    }
    pthread_mutex_unlock(&RUNTIME.lock);
}

static float tuning_rail_command(void)
{
    pthread_mutex_lock(&RUNTIME.lock);
    const float command_v = RUNTIME.rail_command_v;
    pthread_mutex_unlock(&RUNTIME.lock);
    return command_v;
}

static bool frequency_apply_voltage(float target_v, uint32_t generation)
{
    pthread_mutex_lock(&RUNTIME.lock);
    const bool authorized =
        RUNTIME.initialized && RUNTIME.global_state != NULL &&
        !BONANZA_POWER_MANAGEMENT_stop_requested() &&
        !atomic_load_explicit(&RUNTIME.pause_requested,
                              memory_order_acquire) &&
        RUNTIME.supervisor.owner == BZM_SUPERVISOR_OWNER_MINING &&
        RUNTIME.supervisor.report.state == BZM_VALIDATION_HOLDING &&
        !RUNTIME.supervisor.fault_latched &&
        RUNTIME.frequency_target_generation == generation &&
        bzm_supervisor_dispatch_allowed(&RUNTIME.supervisor, now_ms());
    if (!authorized) {
        pthread_mutex_unlock(&RUNTIME.lock);
        return false;
    }

    const float previous_v = RUNTIME.rail_command_v;
    if (fabsf(previous_v - target_v) <=
        BZM_TPS546_VOUT_READBACK_TOLERANCE_V) {
        pthread_mutex_unlock(&RUNTIME.lock);
        return true;
    }

    /*
     * Publish the expected rail atomically with the bounded PMBus operation
     * so the safety monitor cannot compare a new command to stale state.
     */
    RUNTIME.rail_command_v = target_v;
    const bool applied =
        BONANZA_VCORE_bzm_set_runtime_voltage(RUNTIME.global_state, target_v) ==
        ESP_OK;
    if (atomic_load_explicit(&RUNTIME.pause_requested,
                             memory_order_acquire)) {
        pthread_mutex_unlock(&RUNTIME.lock);
        ESP_LOGI(TAG,
                 "BZM live voltage generation %lu was cancelled",
                 (unsigned long)generation);
        return false;
    }
    if (!applied) {
        char detail[BZM_VALIDATION_DETAIL_LENGTH];
        snprintf(detail, sizeof(detail),
                 "BZM runtime rail transition %.3fV -> %.3fV failed",
                 previous_v, target_v);
        close_dispatch_locked();
        (void)bzm_supervisor_latch_fault(
            &RUNTIME.supervisor, 0x1009, detail);
        sync_dispatch_locked();
        pthread_mutex_unlock(&RUNTIME.lock);
        return false;
    }

    RUNTIME.health_sampled_at_ms = 0;
    snprintf(
        RUNTIME.supervisor.report.stages[BZM_STAGE_POWER_RAIL].detail,
        sizeof(RUNTIME.supervisor.report.stages[BZM_STAGE_POWER_RAIL].detail),
        "BONANZA_POWER_RAIL GOOD; user target command %.3f V", target_v);
    pthread_mutex_unlock(&RUNTIME.lock);
    ESP_LOGI(TAG,
             "BZM live mining rail reached user target %.3f V "
             "for generation %lu",
             target_v, (unsigned long)generation);
    return true;
}

static bool frequency_domains_all_at(const bzm_bringup_state_t *state,
                                     float target_mhz)
{
    if (state == NULL) return false;
    for (size_t asic = 0; asic < BZM_BRINGUP_ASIC_COUNT; ++asic) {
        for (size_t pll = 0; pll < BZM_BRINGUP_PLL_COUNT; ++pll) {
            if (fabsf(state->domain_clock_mhz[asic][pll] - target_mhz) >=
                0.001f) {
                return false;
            }
        }
    }
    return true;
}

static void frequency_domains_copy(
    float destination[BZM_BRINGUP_ASIC_COUNT][BZM_BRINGUP_PLL_COUNT],
    const float source[BZM_BRINGUP_ASIC_COUNT][BZM_BRINGUP_PLL_COUNT])
{
    memcpy(destination, source,
           sizeof(float) * BZM_BRINGUP_ASIC_COUNT *
               BZM_BRINGUP_PLL_COUNT);
}

static bool frequency_transition_authorized_locked(uint32_t generation)
{
    return RUNTIME.initialized && RUNTIME.global_state != NULL &&
           !BONANZA_POWER_MANAGEMENT_stop_requested() &&
           !atomic_load_explicit(&RUNTIME.pause_requested,
                                 memory_order_acquire) &&
           RUNTIME.running_evidence_monitoring &&
           RUNTIME.frequency_ramp_active &&
           RUNTIME.frequency_target_generation == generation &&
           RUNTIME.supervisor.owner == BZM_SUPERVISOR_OWNER_MINING &&
           RUNTIME.supervisor.report.state == BZM_VALIDATION_HOLDING &&
           !RUNTIME.supervisor.fault_latched &&
           bzm_supervisor_dispatch_allowed(&RUNTIME.supervisor, now_ms());
}

static bool frequency_apply_domains(
    const float
        frequency_mhz[BZM_BRINGUP_ASIC_COUNT][BZM_BRINGUP_PLL_COUNT],
    bool allow_initial_jump, uint32_t generation, float *actual_mhz)
{
    bzm_bringup_report_t report = {0};
    const bzm_bringup_outcome_t outcome =
        BZM_staged_frequency_domains_step_live(
            frequency_mhz, allow_initial_jump, &report, actual_mhz);

    bzm_running_stats_t baseline = {0};
    const bool baseline_valid =
        outcome != BZM_BRINGUP_GOOD ||
        BZM_running_stats_snapshot(&baseline);

    /* A pause can arrive while the bounded PLL operation owns the driver
     * reactor. It then waits for that operation and forces safe-off. Commit
     * no stale clock/evidence state, and latch no false transition fault,
     * unless this exact tuning generation still owns a healthy mining lease. */
    pthread_mutex_lock(&RUNTIME.lock);
    if (!frequency_transition_authorized_locked(generation)) {
        pthread_mutex_unlock(&RUNTIME.lock);
        ESP_LOGI(TAG,
                 "BZM live frequency generation %lu was cancelled",
                 (unsigned long)generation);
        return false;
    }

    if (outcome != BZM_BRINGUP_GOOD) {
        ESP_LOGE(TAG,
                 "Live domain-frequency transaction failed: reason=%s "
                 "asic=0x%02x pll=%u completed=%u",
                 bzm_bringup_reason_name(report.reason), report.asic_id,
                 report.pll_index, report.completed_items);
        char detail[BZM_VALIDATION_DETAIL_LENGTH];
        snprintf(
            detail, sizeof(detail),
            "live PLL transaction failed: %s ASIC=0x%02x PLL=%u",
            bzm_bringup_reason_name(report.reason), report.asic_id,
            report.pll_index);
        close_dispatch_locked();
        (void)bzm_supervisor_latch_fault(
            &RUNTIME.supervisor, 0x100a, detail);
        sync_dispatch_locked();
        pthread_mutex_unlock(&RUNTIME.lock);
        return false;
    }

    if (!baseline_valid) {
        ESP_LOGE(TAG,
                 "Unable to restart RUNNING evidence after work refresh");
        close_dispatch_locked();
        (void)bzm_supervisor_latch_fault(
            &RUNTIME.supervisor, 0x100a,
            "live PLL transition could not restart mining evidence");
        sync_dispatch_locked();
        pthread_mutex_unlock(&RUNTIME.lock);
        return false;
    }

    RUNTIME.global_state->POWER_MANAGEMENT_MODULE.actual_frequency =
        actual_mhz == NULL ? 0.0f : *actual_mhz;
    RUNTIME.global_state->POWER_MANAGEMENT_MODULE.expected_hashrate =
        expected_bzm_hashrate_ghs(
            RUNTIME.global_state, RUNTIME.frequency_target_mhz);
    bzm_pll_lock_confirmation_init(&RUNTIME.pll_lock_confirmation);
    RUNTIME.health_sampled_at_ms = 0;
    RUNTIME.running_evidence_baseline = baseline;
    bzm_running_evidence_lifecycle_init(
        &RUNTIME.running_evidence_lifecycle);
    RUNTIME.running_evidence_lifecycle.completed_once = true;
    RUNTIME.running_evidence_started_at_ms = now_ms();
    RUNTIME.running_evidence_frequency_transition = true;
    RUNTIME.running_evidence = (bzm_running_evidence_result_t){
        .status = BZM_RUNNING_EVIDENCE_PENDING,
        .fault = BZM_RUNNING_EVIDENCE_FAULT_NONE,
    };
    snprintf(RUNTIME.running_evidence.detail,
             sizeof(RUNTIME.running_evidence.detail),
             "frequency transition: replacing engine work at the new PLL rate");
    pthread_mutex_unlock(&RUNTIME.lock);
    return true;
}

bool BZM_board_apply(void *context, bonanza_power_target_t target)
{
    (void)context;
    {
        float target_mhz = 0.0f;
        float target_voltage_v = 0.0f;
        uint32_t generation = 0;
        if (!tuning_snapshot(
                target, &target_mhz, &target_voltage_v, &generation)) {
            return true;
        }

        if (RUNTIME.replacement_pending) {
            uint32_t current_generation = 0;
            uint32_t completed = 0;
            bool pending = false;
            if (!BZM_work_replacement_snapshot(&current_generation, &completed, &pending)) return false;
            if (pending && (int32_t)(completed - RUNTIME.replacement_generation) < 0) {
                return now_ms() - RUNTIME.replacement_started_ms < BZM_FREQUENCY_TRANSITION_PROOF_TIMEOUT_MS;
            }
            RUNTIME.replacement_pending = false;
        }
        bzm_bringup_state_t state;
        if (!BZM_staged_get_state(&state) || !state.running_verified) {
            return true;
        }

        const float rail_command_v = tuning_rail_command();
        if (target_voltage_v >
            rail_command_v + BZM_TPS546_VOUT_READBACK_TOLERANCE_V) {
            tuning_set_active(true, generation);
            if (!frequency_apply_voltage(target_voltage_v, generation)) {
                tuning_set_active(false, generation);
                return BONANZA_POWER_MANAGEMENT_stop_requested();
            }
            return true;
        }

        if (frequency_domains_all_at(&state, target_mhz)) {
            if (target_voltage_v <
                rail_command_v -
                    BZM_TPS546_VOUT_READBACK_TOLERANCE_V) {
                /* Lower the rail only after the lower clock is reached. */
                tuning_set_active(true, generation);
                if (!frequency_apply_voltage(target_voltage_v, generation)) {
                    tuning_set_active(false, generation);
                    return BONANZA_POWER_MANAGEMENT_stop_requested();
                }
                return true;
            }
            tuning_set_active(false, generation);
            return true;
        }
        tuning_set_active(true, generation);

        if (target_mhz > BZM_FREQUENCY_POWER_ON_MHZ + 0.001f &&
            frequency_domains_all_at(
                &state, BZM_FREQUENCY_POWER_ON_MHZ)) {
            const float initial_mhz =
                bzm_frequency_initial_mhz(target_mhz);
            if (initial_mhz >
                BZM_FREQUENCY_POWER_ON_MHZ + 0.001f) {
                float initial[BZM_BRINGUP_ASIC_COUNT]
                             [BZM_BRINGUP_PLL_COUNT];
                for (size_t asic = 0;
                     asic < BZM_BRINGUP_ASIC_COUNT; ++asic) {
                    for (size_t pll = 0;
                         pll < BZM_BRINGUP_PLL_COUNT; ++pll) {
                        initial[asic][pll] = initial_mhz;
                    }
                }
                float actual_mhz = state.clock_mhz;
                if (!frequency_apply_domains(
                        initial, true, generation, &actual_mhz)) {
                    tuning_set_active(false, generation);
                    return BONANZA_POWER_MANAGEMENT_stop_requested();
                }
                ESP_LOGI(TAG,
                         "BZM live shortcut reached %.3f MHz; continuing "
                         "directly to the user target",
                         initial_mhz);
                uint32_t completed = 0;
                if (!BZM_work_replacement_snapshot(&RUNTIME.replacement_generation,
                        &completed, &RUNTIME.replacement_pending)) return false;
                RUNTIME.replacement_started_ms = now_ms();
                return true;
            }
        }

        float next[BZM_BRINGUP_ASIC_COUNT][BZM_BRINGUP_PLL_COUNT];
        frequency_domains_copy(next, state.domain_clock_mhz);
        bool changing = false;
        for (size_t asic = 0; asic < BZM_BRINGUP_ASIC_COUNT; ++asic) {
            for (size_t pll = 0; pll < BZM_BRINGUP_PLL_COUNT; ++pll) {
                const float current = state.domain_clock_mhz[asic][pll];
                if (current > target_mhz + 0.001f) {
                    next[asic][pll] =
                        fmaxf(target_mhz,
                              current - BZM_FREQUENCY_RAMP_STEP_MHZ);
                    changing = true;
                } else if (current < target_mhz - 0.001f) {
                    next[asic][pll] =
                        fminf(target_mhz,
                              current + BZM_FREQUENCY_RAMP_STEP_MHZ);
                    changing = true;
                }
            }
        }
        if (!changing) {
            tuning_set_active(false, generation);
            return true;
        }

        float actual_mhz = state.clock_mhz;
        if (!frequency_apply_domains(
                next, false, generation, &actual_mhz)) {
            tuning_set_active(false, generation);
            return BONANZA_POWER_MANAGEMENT_stop_requested();
        }
        uint32_t completed = 0;
        if (!BZM_work_replacement_snapshot(&RUNTIME.replacement_generation,
                &completed, &RUNTIME.replacement_pending)) return false;
        RUNTIME.replacement_started_ms = now_ms();
    }
    return true;
}

static bool recoverable_overheat_fault(bzm_runtime_health_fault_t fault)
{
    return fault == BZM_RUNTIME_HEALTH_FAULT_ASIC_OVERHEAT ||
           fault == BZM_RUNTIME_HEALTH_FAULT_TPS_OVERHEAT;
}

static void board_io_task(void *parameter)
{
    (void)parameter;
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(BZM_MONITOR_PERIOD_MS));
        pthread_mutex_lock(&RUNTIME.lock);
        const uint64_t current_ms = now_ms();
        if (runtime_is_holding_locked()) {
            /* No pool/socket calls, lifecycle transitions, NVS writes, or
             * tuning here. This reader outlives stalled share submission. */
            (void)BZM_staged_poll(1);
            if (RUNTIME.health.status == BZM_RUNTIME_HEALTH_BAD ||
                RUNTIME.supervisor.lease_deadline_ms <= current_ms) {
                close_dispatch_locked();
                pthread_mutex_unlock(&RUNTIME.lock);
                continue; /* Do not extend an unhealthy board's output lease. */
            }
            bzm_bridge_safety_status_t status = {0};
            if (BZM_bridge_safety_heartbeat(&status) != ESP_OK ||
                !bridge_status_runtime_good(&status) ||
                status.state != BZM_BRIDGE_SAFETY_STATE_CONTROLLED ||
                status.lease_remaining_ms == 0) {
                RUNTIME.health = (bzm_runtime_health_result_t){
                    .status = BZM_RUNTIME_HEALTH_BAD,
                    .fault = BZM_RUNTIME_HEALTH_FAULT_BRIDGE_UNAVAILABLE,
                };
                snprintf(RUNTIME.health.detail, sizeof(RUNTIME.health.detail),
                         "bridge heartbeat/status interlock failed");
                RUNTIME.health_valid = true;
            } else {
                RUNTIME.bridge_status = status;
                RUNTIME.bridge_status_valid = true;
            }
            if (RUNTIME.health.status != BZM_RUNTIME_HEALTH_BAD &&
                (RUNTIME.health_sampled_at_ms == 0 ||
                 current_ms - RUNTIME.health_sampled_at_ms >= BZM_HEALTH_PERIOD_MS)) {
                (void)sample_runtime_health_locked();
            }
            if (RUNTIME.health.status == BZM_RUNTIME_HEALTH_BAD) {
                /* Revoking work is an interlock; only power management
                 * executes the shutdown or decides whether to recover. */
                close_dispatch_locked();
            }
        }
        pthread_mutex_unlock(&RUNTIME.lock);
    }
}

esp_err_t BZM_board_init(GlobalState * global_state)
{
    if (global_state == NULL)
        return ESP_ERR_INVALID_ARG;
    if (!(global_state->DEVICE_CONFIG.family.id == BONANZA) || global_state->DEVICE_CONFIG.family.asic.id != BZM) {
        return ESP_OK;
    }
    if (!BZM_driver_state_init(global_state) ||
        !runtime_state_init(global_state)) {
        return ESP_ERR_NO_MEM;
    }

    pthread_mutex_lock(&RUNTIME.lock);
    if (RUNTIME.initialized) {
        pthread_mutex_unlock(&RUNTIME.lock);
        return ESP_OK;
    }
    RUNTIME.global_state = global_state;
    RUNTIME.active = true;
    bzm_frequency_target_t configured_target;
    const float requested_mhz =
        (RUNTIME.global_state->SELF_TEST_MODULE.is_active ? 800.0f : nvs_config_get_float(NVS_CONFIG_ASIC_FREQUENCY));
    if (!bzm_frequency_request_is_valid(requested_mhz) ||
        !bzm_frequency_resolve_target(
            requested_mhz, &configured_target)) {
        configured_target = (bzm_frequency_target_t){
            .requested_mhz = BZM_FREQUENCY_POWER_ON_MHZ,
            .actual_mhz = BZM_FREQUENCY_POWER_ON_MHZ,
        };
        ESP_LOGW(TAG,
                 "Invalid saved Bonanza frequency %.3f MHz; using 800 MHz",
                 requested_mhz);
    }
    RUNTIME.frequency_target_mhz = configured_target.actual_mhz;
    const uint16_t requested_voltage_mv =
        (RUNTIME.global_state->SELF_TEST_MODULE.is_active ? 2800 : nvs_config_get_u16(NVS_CONFIG_ASIC_VOLTAGE));
    float requested_voltage_v = 0.0f;
    if (bzm_power_resolve_user_voltage(
            requested_voltage_mv, &requested_voltage_v)) {
        RUNTIME.voltage_target_v = requested_voltage_v;
    } else {
        RUNTIME.voltage_target_v = BZM_TPS546_FIXED_VOUT_V;
        ESP_LOGW(TAG,
                 "Invalid saved Bonanza rail voltage %u mV; using 2800 mV",
                 (unsigned)requested_voltage_mv);
    }
    RUNTIME.frequency_target_generation = 1;
    RUNTIME.frequency_ramp_active =
        fabsf(configured_target.actual_mhz -
              BZM_FREQUENCY_POWER_ON_MHZ) >= 0.001f ||
        fabsf(RUNTIME.voltage_target_v -
              BZM_TPS546_FIXED_VOUT_V) >= 0.001f;
    RUNTIME.rail_command_v = BZM_TPS546_FIXED_VOUT_V;
    global_state->POWER_MANAGEMENT_MODULE.frequency_value =
        configured_target.actual_mhz;
    global_state->POWER_MANAGEMENT_MODULE.actual_frequency = 0.0f;
    global_state->POWER_MANAGEMENT_MODULE.expected_hashrate =
        expected_bzm_hashrate_ghs(
            global_state, configured_target.actual_mhz);
    reset_running_evidence_locked(false);
    bzm_ch2_confirmation_init(&RUNTIME.ch2_confirmation);
    bzm_pll_lock_confirmation_init(&RUNTIME.pll_lock_confirmation);
    atomic_init(&RUNTIME.dispatch_enabled, false);
    atomic_init(&RUNTIME.pause_requested, false);
    atomic_init(&RUNTIME.dispatch_deadline_ms, 0);
    atomic_init(&RUNTIME.execution_deadline_ms, 0);
    atomic_init(&RUNTIME.execution_cancelled, false);
    BZM_staged_set_operation_authorizer(runtime_execution_authorizer, &RUNTIME);

    bzm_validation_ops_t ops = {
        .run_stage = runtime_run_stage,
        .force_safe_off = runtime_force_safe_off,
    };
    bzm_supervisor_config_t config = {
        .build_max_stage = BZM_STAGE_RUNNING,
        .implemented_max_stage = BZM_STAGE_RUNNING,
        .powered_stages_compiled = true,
        .production_mode = true,
        .independent_kill_available = false,
        .allow_esp_only_kill_in_lab = false,
        .board_managed_safety = true,
        .maximum_lease_ms = BZM_BOARD_WATCHDOG_MS,
    };
    if (!bzm_supervisor_init(&RUNTIME.supervisor, &config, &ops, global_state)) {
        pthread_mutex_unlock(&RUNTIME.lock);
        return ESP_ERR_INVALID_STATE;
    }
    RUNTIME.initialized = true;
    refresh_bridge_evidence_locked();
    bool safe = bzm_supervisor_request_validation(&RUNTIME.supervisor, BZM_STAGE_OFF_SAFE, false, false, 0, now_ms());
    pthread_mutex_unlock(&RUNTIME.lock);
    if (!safe)
        return ESP_FAIL;

    if (xTaskCreate(board_io_task, "bzm_io", 6144, NULL,
                    BZM_IO_TASK_PRIORITY, NULL) != pdPASS) {
        pthread_mutex_lock(&RUNTIME.lock);
        close_dispatch_locked();
        (void) bzm_supervisor_latch_fault(&RUNTIME.supervisor, 0x1002, "Bonanza safety monitor task could not start");
        pthread_mutex_unlock(&RUNTIME.lock);
        return ESP_ERR_NO_MEM;
    }
    pthread_mutex_lock(&RUNTIME.lock);
    RUNTIME.monitor_running = true;
    pthread_mutex_unlock(&RUNTIME.lock);
    ESP_LOGI(TAG, "Bonanza board operations initialized at safe-off");
    return ESP_OK;
}

bool BZM_board_stop(void *context)
{
    (void)context;
    if (RUNTIME_STATE == NULL) return false;
    /* Publish intent before waiting for either the board lock or the
     * BZM reactor. A live PLL/rail transaction that completes concurrently
     * must yield to pause instead of latching its cancellation as a fault. */
    atomic_store_explicit(&RUNTIME.pause_requested, true,
                          memory_order_release);
    pthread_mutex_lock(&RUNTIME.lock);
    if (!RUNTIME.active) {
        pthread_mutex_unlock(&RUNTIME.lock);
        return true;
    }
    if (!RUNTIME.initialized || RUNTIME.global_state == NULL ||
        bzm_supervisor_owner_is_maintenance(RUNTIME.supervisor.owner)) {
        pthread_mutex_unlock(&RUNTIME.lock);
        return false;
    }

    close_dispatch_locked();
    RUNTIME.frequency_ramp_active = false;
    ++RUNTIME.frequency_target_generation;
    reset_running_evidence_locked(false);

    bool safe = bzm_supervisor_safe_off_verified(&RUNTIME.supervisor) &&
                RUNTIME.supervisor.owner == BZM_SUPERVISOR_OWNER_NONE;
    if (!safe) {
        safe = RUNTIME.supervisor.owner == BZM_SUPERVISOR_OWNER_NONE
                   ? bzm_supervisor_request_validation(
                         &RUNTIME.supervisor, BZM_STAGE_OFF_SAFE,
                         false, false, 0, now_ms())
                   : bzm_supervisor_stop(
                         &RUNTIME.supervisor, "operator paused mining");
        safe = safe &&
               bzm_supervisor_safe_off_verified(&RUNTIME.supervisor);
    }

    RUNTIME.global_state->ASIC_initalized = false;
    RUNTIME.global_state->POWER_MANAGEMENT_MODULE.actual_frequency = 0.0f;
    RUNTIME.global_state->POWER_MANAGEMENT_MODULE.expected_hashrate = 0.0f;
    if (safe) {
        RUNTIME.rail_command_v = BZM_TPS546_FIXED_VOUT_V;
        RUNTIME.global_state->POWER_MANAGEMENT_MODULE.core_voltage = 0.0f;
        RUNTIME.global_state->POWER_MANAGEMENT_MODULE.current = 0.0f;
        RUNTIME.global_state->POWER_MANAGEMENT_MODULE.power = 0.0f;
    }
    sync_dispatch_locked();
    pthread_mutex_unlock(&RUNTIME.lock);

    if (safe) {
        ESP_LOGI(TAG, "Bonanza mining paused at verified OFF_SAFE");
    } else {
        ESP_LOGE(TAG,
                 "Bonanza mining pause failed closed without verified OFF_SAFE");
    }
    return safe;
}

static bool board_start(void *context)
{
    (void)context;
    if (RUNTIME_STATE == NULL) return false;
    pthread_mutex_lock(&RUNTIME.lock);
    if (!RUNTIME.active) {
        pthread_mutex_unlock(&RUNTIME.lock);
        return true;
    }
    RUNTIME.mining_stack_ready = true;
    if (RUNTIME.supervisor.fault_latched &&
        bzm_supervisor_request_validation(&RUNTIME.supervisor, BZM_STAGE_OFF_SAFE,
                                           false, false, 0, now_ms())) {
        (void)bzm_supervisor_clear_fault(&RUNTIME.supervisor);
    }
    RUNTIME.health = (bzm_runtime_health_result_t){0};
    atomic_store_explicit(&RUNTIME.execution_cancelled, false, memory_order_release);
    RUNTIME.replacement_pending = false;
    if (!RUNTIME.initialized || RUNTIME.global_state == NULL ||
        !RUNTIME.mining_stack_ready ||
        bzm_supervisor_owner_is_maintenance(RUNTIME.supervisor.owner) ||
        RUNTIME.supervisor.fault_latched) {
        pthread_mutex_unlock(&RUNTIME.lock);
        return false;
    }

    if (RUNTIME.supervisor.owner == BZM_SUPERVISOR_OWNER_MINING &&
        bzm_supervisor_dispatch_allowed(&RUNTIME.supervisor, now_ms())) {
        atomic_store_explicit(&RUNTIME.pause_requested, false,
                              memory_order_release);
        pthread_mutex_unlock(&RUNTIME.lock);
        return true;
    }
    if (RUNTIME.supervisor.owner != BZM_SUPERVISOR_OWNER_NONE ||
        !bzm_supervisor_safe_off_verified(&RUNTIME.supervisor)) {
        pthread_mutex_unlock(&RUNTIME.lock);
        return false;
    }

    bzm_frequency_target_t frequency_target;
    float voltage_target_v = 0.0f;
    if (!configured_tuning_target(
            &frequency_target, &voltage_target_v)) {
        pthread_mutex_unlock(&RUNTIME.lock);
        ESP_LOGE(TAG,
                 "Bonanza resume rejected invalid saved frequency or voltage");
        return false;
    }

    RUNTIME.frequency_target_mhz = frequency_target.actual_mhz;
    RUNTIME.voltage_target_v = voltage_target_v;
    ++RUNTIME.frequency_target_generation;
    RUNTIME.frequency_ramp_active =
        fabsf(frequency_target.actual_mhz -
              BZM_FREQUENCY_POWER_ON_MHZ) >= 0.001f ||
        fabsf(voltage_target_v - BZM_TPS546_FIXED_VOUT_V) >= 0.001f;
    RUNTIME.rail_command_v = BZM_TPS546_FIXED_VOUT_V;
    RUNTIME.health_sampled_at_ms = 0;
    RUNTIME.global_state->POWER_MANAGEMENT_MODULE.frequency_value =
        frequency_target.actual_mhz;
    RUNTIME.global_state->POWER_MANAGEMENT_MODULE.actual_frequency = 0.0f;
    RUNTIME.global_state->POWER_MANAGEMENT_MODULE.expected_hashrate =
        expected_bzm_hashrate_ghs(
            RUNTIME.global_state, frequency_target.actual_mhz);

    atomic_store_explicit(&RUNTIME.pause_requested, false,
                          memory_order_release);
    const bool started = start_production_mining_locked();
    if (!started) {
        atomic_store_explicit(&RUNTIME.pause_requested, true,
                              memory_order_release);
        RUNTIME.frequency_ramp_active = false;
        RUNTIME.global_state->POWER_MANAGEMENT_MODULE.actual_frequency =
            0.0f;
        RUNTIME.global_state->POWER_MANAGEMENT_MODULE.expected_hashrate =
            0.0f;
    }
    pthread_mutex_unlock(&RUNTIME.lock);

    if (started) {
        ESP_LOGI(TAG,
                 "Bonanza mining resumed at 800 MHz; live target %.3f MHz",
                 frequency_target.actual_mhz);
    } else {
        ESP_LOGE(TAG, "Bonanza mining resume failed closed");
    }
    return started;
}

static bool board_acquire_maintenance(bzm_supervisor_owner_t owner)
{
    if (RUNTIME_STATE == NULL) return false;
    atomic_store_explicit(&RUNTIME.pause_requested, true,
                          memory_order_release);
    pthread_mutex_lock(&RUNTIME.lock);
    close_dispatch_locked();
    bool ok = RUNTIME.initialized && bzm_supervisor_acquire_maintenance(&RUNTIME.supervisor, owner, now_ms());
    sync_dispatch_locked();
    pthread_mutex_unlock(&RUNTIME.lock);
    return ok;
}

static bool board_release_maintenance(bzm_supervisor_owner_t owner)
{
    if (RUNTIME_STATE == NULL) return false;
    pthread_mutex_lock(&RUNTIME.lock);
    close_dispatch_locked();
    bool ok = RUNTIME.initialized && bzm_supervisor_release_maintenance(&RUNTIME.supervisor, owner);
    sync_dispatch_locked();
    pthread_mutex_unlock(&RUNTIME.lock);
    return ok;
}

static bool board_prepare_restart(void)
{
    if (RUNTIME_STATE == NULL) return false;
    atomic_store_explicit(&RUNTIME.pause_requested, true,
                          memory_order_release);
    pthread_mutex_lock(&RUNTIME.lock);
    if (!RUNTIME.active) {
        pthread_mutex_unlock(&RUNTIME.lock);
        return true;
    }

    close_dispatch_locked();
    bool ok = RUNTIME.initialized && bzm_supervisor_prepare_restart(&RUNTIME.supervisor);
    sync_dispatch_locked();
    pthread_mutex_unlock(&RUNTIME.lock);
    return ok;
}

bool BZM_board_maintenance(void *context, bonanza_power_owner_t owner, bool acquire)
{
    (void)context;
    if (owner == BONANZA_POWER_OWNER_RESTART) return acquire ? board_prepare_restart()
        : board_release_maintenance(BZM_SUPERVISOR_OWNER_ESP_RESTART);
    if (owner != BONANZA_POWER_OWNER_OTA) return false;
    return acquire ? board_acquire_maintenance(BZM_SUPERVISOR_OWNER_ESP_OTA)
                   : board_release_maintenance(BZM_SUPERVISOR_OWNER_ESP_OTA);
}

bonanza_power_sample_t BZM_board_sample(void)
{
    bonanza_power_sample_t result = {.health = BONANZA_POWER_HEALTH_FAULT};
    if (RUNTIME_STATE == NULL || !RUNTIME.initialized) return result;
    pthread_mutex_lock(&RUNTIME.lock);
    const uint64_t current_ms = now_ms();
    if (RUNTIME.supervisor.owner == BZM_SUPERVISOR_OWNER_MINING) {
        if (!bzm_supervisor_heartbeat(&RUNTIME.supervisor,
                RUNTIME.supervisor.config.maximum_lease_ms, current_ms)) {
            RUNTIME.health.status = BZM_RUNTIME_HEALTH_BAD;
            RUNTIME.health.fault = BZM_RUNTIME_HEALTH_FAULT_BRIDGE_LEASE;
            snprintf(RUNTIME.health.detail, sizeof(RUNTIME.health.detail),
                     "power-management watchdog expired");
        }
        if (RUNTIME.running_evidence_monitoring &&
            evaluate_running_evidence_locked(current_ms).status == BZM_RUNNING_EVIDENCE_BAD) {
            RUNTIME.health.status = BZM_RUNTIME_HEALTH_BAD;
            RUNTIME.health.fault = BZM_RUNTIME_HEALTH_FAULT_INVALID_INPUT;
            snprintf(RUNTIME.health.detail, sizeof(RUNTIME.health.detail), "%s",
                     RUNTIME.running_evidence.detail);
        }
    }
    result.health = RUNTIME.supervisor.fault_latched ? BONANZA_POWER_HEALTH_FAULT : BONANZA_POWER_HEALTH_OK;
    if (RUNTIME.health.status == BZM_RUNTIME_HEALTH_BAD) {
        result.health = recoverable_overheat_fault(RUNTIME.health.fault)
            ? BONANZA_POWER_HEALTH_OVERHEAT : BONANZA_POWER_HEALTH_FAULT;
    }
    snprintf(result.detail, sizeof(result.detail), "%s",
        RUNTIME.health.status == BZM_RUNTIME_HEALTH_BAD ? RUNTIME.health.detail :
        RUNTIME.supervisor.fault_latched ? RUNTIME.supervisor.fault_detail : "");
    BONANZA_TPS546_StatusSnapshot power = {0};
    bool pgood = true;
    result.vreg_valid = BONANZA_VCORE_bzm_snapshot(&power, &pgood) == ESP_OK;
    result.vreg_c = power.read_temp1;
    if (result.vreg_valid && RUNTIME.global_state != NULL)
        RUNTIME.global_state->POWER_MANAGEMENT_MODULE.vr_temp = result.vreg_c;
    sync_dispatch_locked();
    pthread_mutex_unlock(&RUNTIME.lock);
    return result;
}

bonanza_power_start_result_t BZM_board_start(void *context)
{
    if (board_start(context)) return BONANZA_POWER_START_OK;
    if (RUNTIME_STATE == NULL) return BONANZA_POWER_START_FAILED;
    pthread_mutex_lock(&RUNTIME.lock);
    bool hot = RUNTIME.health.status == BZM_RUNTIME_HEALTH_BAD &&
               recoverable_overheat_fault(RUNTIME.health.fault);
    pthread_mutex_unlock(&RUNTIME.lock);
    return hot ? BONANZA_POWER_START_OVERHEAT : BONANZA_POWER_START_FAILED;
}
