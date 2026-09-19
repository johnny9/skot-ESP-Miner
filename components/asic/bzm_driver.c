#include "bzm_driver.h"

#include <pthread.h>
#include <stdatomic.h>

#include "bzm_bridge.h"
#include "bzm_balanced_ramp.h"
#include "bzm_bringup.h"
#include "bzm_dispatch_gate.h"
#include "bzm_lease_guard.h"
#include "bzm_reactor.h"
#include "bzm_result.h"
#include "bzm_result_dedup.h"
#include "bzm_registers.h"
#include "bzm_runtime_health.h"
#include "bzm_transport.h"
#include "esp_heap_caps.h"
#include "esp_psram.h"
#include "esp_log.h"
#include "esp_rom_sys.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "global_state.h"
#include "bzm_serial.h"

static const char * TAG = "bzm";

#define BZM_FAST_JOB_INTERVAL_MS 10.0
#define BZM_STEADY_JOB_INTERVAL_MS 100.0

/* A 36-bit filter reports difficulty-16 results. Derive local validation
 * and hashrate credits from the same setting programmed into the engines. */
#define BZM_RESULT_LEAD_ZEROS 36U
#define BZM_RESULT_DIFFICULTY (UINT64_C(1) << (BZM_RESULT_LEAD_ZEROS - 32U))
#define BZM_DISPATCH_GAP_US 250U

typedef struct {
    bzm_job_store_t job_store;
    atomic_uint work_epoch;
    bzm_reactor_t reactor;
    bzm_serial_transport_t transport;
    pthread_mutex_t reactor_lock;
    bool initialized;
    bool staged_transport_ready;
    bool staged_lease_io_ok;
    bzm_lease_guard_schedule_t staged_lease_schedule;
    bzm_dispatch_authorizer_t staged_operation_authorize;
    void *staged_operation_authorize_context;
    bzm_bringup_state_t staged_bringup;
    bzm_balanced_ramp_t staged_balanced_ramp;
    bzm_serial_parser_stats_t staged_engine_window_baseline;
    bzm_serial_parser_stats_t staged_sensor_parser_baseline;
    bool staged_sensor_parser_baseline_valid;
    bzm_serial_parser_stats_t staged_running_parser_baseline;
    bool staged_running_parser_baseline_valid;
    bool staged_engine_window_active;
    bzm_bringup_telemetry_policy_t staged_ramp_telemetry_policy;
    bool staged_ramp_telemetry_policy_ready;
    bzm_telemetry_store_t staged_batch_telemetry;
    bool staged_batch_telemetry_active;
    bzm_dispatch_gate_t staged_dispatch_gate;
    float last_temperature;
    int64_t last_temperature_us;
    atomic_uint_fast64_t running_dispatch_batches;
    atomic_uint_fast64_t running_dispatched_logical_engines;
    atomic_uint_fast64_t running_dispatched_chip_engines;
    atomic_uint_fast64_t running_dispatch_failures;
    atomic_uint_fast64_t running_mapped_results;
    atomic_uint_fast64_t running_mapping_rejections;
    atomic_uint_fast32_t running_mapping_rejection_streak;
    atomic_bool running_mapping_recovery_pending;
    atomic_uint_fast64_t running_locally_valid_results;
    atomic_uint_fast64_t running_locally_rejected_results;
    atomic_uint_fast64_t running_duplicate_results;
    atomic_uint_fast64_t
        hashrate_difficulty_one_counters[BZM_MAX_ASIC_COUNT];
    atomic_uint_fast32_t running_local_rejection_streak;
    atomic_bool running_local_recovery_pending;
    atomic_uint_fast32_t work_replacement_generation;
    uint32_t work_replacement_completed; // Protected by REACTOR_LOCK.
    uint16_t fast_dispatch_remaining;
    bzm_result_dedup_t result_dedup;
} bzm_driver_state_t;

static bzm_driver_state_t *BZM_STATE;

#define REACTOR (*BZM_STATE).reactor
#define TRANSPORT (*BZM_STATE).transport
#define REACTOR_LOCK (*BZM_STATE).reactor_lock
#define INITIALIZED (*BZM_STATE).initialized
#define STAGED_TRANSPORT_READY (*BZM_STATE).staged_transport_ready
#define STAGED_LEASE_IO_OK (*BZM_STATE).staged_lease_io_ok
#define STAGED_LEASE_SCHEDULE (*BZM_STATE).staged_lease_schedule
#define STAGED_OPERATION_AUTHORIZE (*BZM_STATE).staged_operation_authorize
#define STAGED_OPERATION_AUTHORIZE_CONTEXT \
    (*BZM_STATE).staged_operation_authorize_context
#define STAGED_BRINGUP (*BZM_STATE).staged_bringup
#define STAGED_BALANCED_RAMP (*BZM_STATE).staged_balanced_ramp
#define STAGED_ENGINE_WINDOW_BASELINE \
    (*BZM_STATE).staged_engine_window_baseline
#define STAGED_SENSOR_PARSER_BASELINE \
    (*BZM_STATE).staged_sensor_parser_baseline
#define STAGED_SENSOR_PARSER_BASELINE_VALID \
    (*BZM_STATE).staged_sensor_parser_baseline_valid
#define STAGED_RUNNING_PARSER_BASELINE \
    (*BZM_STATE).staged_running_parser_baseline
#define STAGED_RUNNING_PARSER_BASELINE_VALID \
    (*BZM_STATE).staged_running_parser_baseline_valid
#define STAGED_ENGINE_WINDOW_ACTIVE (*BZM_STATE).staged_engine_window_active
#define STAGED_RAMP_TELEMETRY_POLICY \
    (*BZM_STATE).staged_ramp_telemetry_policy
#define STAGED_RAMP_TELEMETRY_POLICY_READY \
    (*BZM_STATE).staged_ramp_telemetry_policy_ready
#define STAGED_BATCH_TELEMETRY (*BZM_STATE).staged_batch_telemetry
#define STAGED_BATCH_TELEMETRY_ACTIVE \
    (*BZM_STATE).staged_batch_telemetry_active
#define STAGED_DISPATCH_GATE (*BZM_STATE).staged_dispatch_gate
#define LAST_TEMPERATURE (*BZM_STATE).last_temperature
#define LAST_TEMPERATURE_US (*BZM_STATE).last_temperature_us
#define RUNNING_DISPATCH_BATCHES (*BZM_STATE).running_dispatch_batches
#define RUNNING_DISPATCHED_LOGICAL_ENGINES \
    (*BZM_STATE).running_dispatched_logical_engines
#define RUNNING_DISPATCHED_CHIP_ENGINES \
    (*BZM_STATE).running_dispatched_chip_engines
#define RUNNING_DISPATCH_FAILURES (*BZM_STATE).running_dispatch_failures
#define RUNNING_MAPPED_RESULTS (*BZM_STATE).running_mapped_results
#define RUNNING_MAPPING_REJECTIONS (*BZM_STATE).running_mapping_rejections
#define RUNNING_MAPPING_REJECTION_STREAK \
    (*BZM_STATE).running_mapping_rejection_streak
#define RUNNING_MAPPING_RECOVERY_PENDING \
    (*BZM_STATE).running_mapping_recovery_pending
#define RUNNING_LOCALLY_VALID_RESULTS \
    (*BZM_STATE).running_locally_valid_results
#define RUNNING_LOCALLY_REJECTED_RESULTS \
    (*BZM_STATE).running_locally_rejected_results
#define RUNNING_DUPLICATE_RESULTS (*BZM_STATE).running_duplicate_results
#define HASHRATE_DIFFICULTY_ONE_COUNTERS \
    (*BZM_STATE).hashrate_difficulty_one_counters
#define RUNNING_LOCAL_REJECTION_STREAK \
    (*BZM_STATE).running_local_rejection_streak
#define RUNNING_LOCAL_RECOVERY_PENDING \
    (*BZM_STATE).running_local_recovery_pending
#define WORK_REPLACEMENT_GENERATION \
    (*BZM_STATE).work_replacement_generation
#define FAST_DISPATCH_REMAINING (*BZM_STATE).fast_dispatch_remaining
#define RESULT_DEDUP (*BZM_STATE).result_dedup

enum
{
    BZM_PARSER_SETTLE_WINDOW_MS = 100,
    BZM_PARSER_SETTLE_REQUIRED_CLEAN_WINDOWS = 10,
    BZM_PARSER_SETTLE_MAX_WINDOWS = 30,
};

static bool staged_settle_parser(bzm_serial_transport_t * transport, const char * stage_name,
                                 const bzm_serial_parser_stats_t * initial_baseline,
                                 bzm_serial_parser_stats_t * accepted_baseline);
static void staged_sleep(void *context, uint32_t delay_ms);

bool BZM_driver_state_init(GlobalState *state)
{
    if (state == NULL || state->DEVICE_CONFIG.family.asic.id != BZM) {
        return false;
    }
    if (BZM_STATE != NULL) return true;
    if (!esp_psram_is_initialized()) return false;

    bzm_driver_state_t *allocated = heap_caps_calloc(
        1, sizeof(*allocated), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (allocated == NULL) {
        ESP_LOGE(TAG, "Unable to allocate Bonanza driver state in PSRAM");
        return false;
    }
    if (pthread_mutex_init(&allocated->reactor_lock, NULL) != 0) {
        heap_caps_free(allocated);
        return false;
    }
    allocated->last_temperature = -1.0f;
    atomic_init(&allocated->work_epoch, 0);
    atomic_init(&allocated->running_dispatch_batches, 0);
    atomic_init(&allocated->running_dispatched_logical_engines, 0);
    atomic_init(&allocated->running_dispatched_chip_engines, 0);
    atomic_init(&allocated->running_dispatch_failures, 0);
    atomic_init(&allocated->running_mapped_results, 0);
    atomic_init(&allocated->running_mapping_rejections, 0);
    atomic_init(&allocated->running_mapping_rejection_streak, 0);
    atomic_init(&allocated->running_mapping_recovery_pending, false);
    atomic_init(&allocated->running_locally_valid_results, 0);
    atomic_init(&allocated->running_locally_rejected_results, 0);
    atomic_init(&allocated->running_duplicate_results, 0);
    atomic_init(&allocated->running_local_rejection_streak, 0);
    atomic_init(&allocated->running_local_recovery_pending, false);
    atomic_init(&allocated->work_replacement_generation, 0);
    for (size_t asic = 0; asic < BZM_MAX_ASIC_COUNT; ++asic) {
        atomic_init(&allocated->hashrate_difficulty_one_counters[asic], 0);
    }

    if (!bzm_job_store_init_with_caps(&allocated->job_store,
            MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)) {
        pthread_mutex_destroy(&allocated->reactor_lock);
        heap_caps_free(allocated);
        return false;
    }
    BZM_STATE = allocated;
    ESP_LOGI(TAG, "Allocated %u bytes of Bonanza driver state in PSRAM",
             (unsigned)sizeof(*allocated));
    return true;
}

static void reset_result_dedup(void)
{
    memset(&RESULT_DEDUP, 0, sizeof(RESULT_DEDUP));
}

static void start_fast_dispatch_locked(uint16_t remaining)
{
    FAST_DISPATCH_REMAINING = remaining;
    if (remaining != 0) {
        atomic_fetch_add_explicit(
            &WORK_REPLACEMENT_GENERATION, 1, memory_order_relaxed);
    }
}

static bool result_is_duplicate(const bzm_result_t *result)
{
    return result->job_valid &&
           bzm_result_is_duplicate(&RESULT_DEDUP, &result->job, result);
}

static void staged_report(bzm_bringup_report_t * report, bzm_bringup_outcome_t outcome, bzm_bringup_reason_t reason)
{
    if (report != NULL) {
        *report = (bzm_bringup_report_t){
            .outcome = outcome,
            .reason = reason,
        };
    }
}

int BZM_set_max_baud(void)
{
    return BZM_SERIAL_set_baud(BZM_BAUD_RATE) == ESP_OK ? BZM_BAUD_RATE : 0;
}

static bool BZM_send_work(GlobalState * state, const asic_job_t * template)
{
    (void) state;
    if (BZM_STATE == NULL || !INITIALIZED || template == NULL) {
        return false;
    }

    if (STAGED_TRANSPORT_READY) {
        pthread_mutex_lock(&REACTOR_LOCK);
        bool running = STAGED_BRINGUP.running_verified;
        bzm_dispatch_gate_t gate = STAGED_DISPATCH_GATE;
        pthread_mutex_unlock(&REACTOR_LOCK);
        if (!running || !bzm_dispatch_gate_is_authorized(&gate))
            return false;
    }

    pthread_mutex_lock(&REACTOR_LOCK);
    bool results_were_quarantined =
        bzm_reactor_results_quarantined(&REACTOR);

    bzm_work_t assigned_work;
    bzm_assign_status_t status =
        bzm_reactor_assign(&REACTOR, template, &assigned_work);
    if (status == BZM_ASSIGN_FLUSH_REQUIRED) {
        if (!bzm_reactor_begin_flush(&REACTOR)) {
            pthread_mutex_unlock(&REACTOR_LOCK);
            atomic_fetch_add_explicit(&RUNNING_DISPATCH_FAILURES, 1, memory_order_relaxed);
            ESP_LOGE(TAG, "Unable to complete BZM flush barrier");
            return false;
        }
        bzm_reactor_finish_flush(&REACTOR);
        // Hardware sequence reuse does not make an already seen share new.
        start_fast_dispatch_locked(REACTOR.config.engine_count);
        results_were_quarantined = true;
        status = bzm_reactor_assign(&REACTOR, template, &assigned_work);
    }
    bool rotation_complete = status == BZM_ASSIGN_OK &&
        REACTOR.next_engine == 0;
    size_t chip_engines = status == BZM_ASSIGN_OK
        ? TRANSPORT.asic_count : 0;
    bool fast_dispatch_complete = status == BZM_ASSIGN_OK &&
        FAST_DISPATCH_REMAINING == 1;
    if (fast_dispatch_complete)
        BZM_STATE->work_replacement_completed = (uint32_t)atomic_load_explicit(
            &WORK_REPLACEMENT_GENERATION, memory_order_relaxed);
    if (status == BZM_ASSIGN_OK && FAST_DISPATCH_REMAINING != 0)
        --FAST_DISPATCH_REMAINING;
    if (status == BZM_ASSIGN_OK && results_were_quarantined &&
        !bzm_reactor_results_quarantined(&REACTOR)) {
        /* A clean Stratum job makes every pre-flush result unsubmitable.
         * Drain anything queued during the bounded replacement rotation
         * before normal attribution resumes. */
        bzm_serial_discard_pending_results(&TRANSPORT);
    }
    /* Pause and other supervisor transitions withdraw dispatch permission
     * asynchronously so a long engine rotation stops at its next checkpoint.
     * The reactor reports that deliberate cancellation as a transport error;
     * keep returning false to stop the caller, but do not turn the requested
     * safe-off transition into a runtime hardware fault. */
    bool dispatch_cancelled = status != BZM_ASSIGN_OK &&
        STAGED_TRANSPORT_READY &&
        !bzm_dispatch_gate_is_authorized(&STAGED_DISPATCH_GATE);
    pthread_mutex_unlock(&REACTOR_LOCK);

    if (status != BZM_ASSIGN_OK) {
        if (dispatch_cancelled) {
            ESP_LOGI(TAG,
                     "BZM work assignment cancelled after dispatch authorization was withdrawn");
            return false;
        }
        atomic_fetch_add_explicit(&RUNNING_DISPATCH_FAILURES, 1, memory_order_relaxed);
        ESP_LOGE(TAG, "BZM work assignment failed (%d)", status);
        return false;
    }
    if (rotation_complete) {
        atomic_fetch_add_explicit(&RUNNING_DISPATCH_BATCHES, 1,
                                  memory_order_relaxed);
    }
    if (fast_dispatch_complete) {
        ESP_LOGI(TAG,
                 "BZM independent engine rotation complete; steady refresh interval %.0f ms",
                 BZM_STEADY_JOB_INTERVAL_MS);
    }
    atomic_fetch_add_explicit(&RUNNING_DISPATCHED_LOGICAL_ENGINES, 1,
                              memory_order_relaxed);
    atomic_fetch_add_explicit(&RUNNING_DISPATCHED_CHIP_ENGINES, chip_engines, memory_order_relaxed);
    return true;
}

bool BZM_clear_work(GlobalState * state)
{
    if (BZM_STATE == NULL) return true;
    atomic_fetch_add_explicit(&BZM_STATE->work_epoch, 1, memory_order_acq_rel);
    (void) state;
    pthread_mutex_lock(&REACTOR_LOCK);
    bool cleared = !INITIALIZED || bzm_reactor_invalidate_work(&REACTOR);
    if (cleared) {
        reset_result_dedup();
        if (INITIALIZED && FAST_DISPATCH_REMAINING == 0)
            start_fast_dispatch_locked(REACTOR.config.engine_count);
    }
    pthread_mutex_unlock(&REACTOR_LOCK);
    if (!cleared) {
        atomic_fetch_add_explicit(&RUNNING_DISPATCH_FAILURES, 1,
                                  memory_order_relaxed);
        ESP_LOGE(TAG, "BZM clean-job barrier failed");
    }
    return cleared;
}

double BZM_job_frequency_ms(GlobalState *state)
{
    if (BZM_STATE == NULL) return BZM_FAST_JOB_INTERVAL_MS;
    (void)state;
    pthread_mutex_lock(&REACTOR_LOCK);
    bool fast_dispatch = INITIALIZED && FAST_DISPATCH_REMAINING != 0;
    pthread_mutex_unlock(&REACTOR_LOCK);
    return fast_dispatch ? BZM_FAST_JOB_INTERVAL_MS
                         : BZM_STEADY_JOB_INTERVAL_MS;
}

static bzm_result_t * BZM_process_result(GlobalState * state)
{
    if (BZM_STATE == NULL) return NULL;
    (void) state;
    if (!INITIALIZED || (STAGED_TRANSPORT_READY && !STAGED_BRINGUP.running_verified)) {
        return NULL;
    }

    bzm_raw_result_t raw;
    static bzm_result_t event;
    pthread_mutex_lock(&REACTOR_LOCK);
    bool received = bzm_serial_read_result(&TRANSPORT, &raw, 20);
    if (received && bzm_reactor_results_quarantined(&REACTOR)) {
        pthread_mutex_unlock(&REACTOR_LOCK);
        vTaskDelay(1);
        return NULL;
    }
    bool nonce_frame = received && bzm_raw_result_has_valid_nonce(&raw);
    bool mapped = nonce_frame && bzm_reactor_map_result(&REACTOR, &raw, &event);
    bool stale = nonce_frame && !mapped &&
        bzm_reactor_result_is_stale(&REACTOR, &raw);
    bool duplicate = mapped && result_is_duplicate(&event);
    pthread_mutex_unlock(&REACTOR_LOCK);
    if (duplicate) {
        atomic_fetch_add_explicit(&RUNNING_DUPLICATE_RESULTS, 1,
                                  memory_order_relaxed);
        vTaskDelay(1);
        return NULL;
    }
    if (mapped) {
        atomic_fetch_add_explicit(&RUNNING_MAPPED_RESULTS, 1, memory_order_relaxed);
    } else if (nonce_frame && !stale) {
        /* An unchecksummed frame that looks like a nonce can contain a
         * corrupted ASIC/engine/sequence field. Recovery is proven only when
         * a later mapped result passes full local hash validation. */
        atomic_store_explicit(&RUNNING_MAPPING_RECOVERY_PENDING, true,
                              memory_order_seq_cst);
        atomic_fetch_add_explicit(&RUNNING_MAPPING_REJECTIONS, 1,
                                  memory_order_seq_cst);
        atomic_fetch_add_explicit(&RUNNING_MAPPING_REJECTION_STREAK, 1,
                                  memory_order_seq_cst);
    }
    /* The lower-priority Stratum task must be able to acquire REACTOR_LOCK
     * to retire a disconnected pool. Repeated timed reads followed by an
     * immediate lock reacquisition otherwise starve that transition when
     * all arriving results are stale. Block outside the lock for one tick;
     * taskYIELD alone cannot schedule a lower-priority waiter. */
    if (!mapped)
        vTaskDelay(1);
    return mapped ? &event : NULL;
}

bool BZM_running_stats_snapshot(bzm_running_stats_t * stats)
{
    if (stats == NULL)
        return false;
    *stats = (bzm_running_stats_t){
        .dispatch_batches = atomic_load_explicit(&RUNNING_DISPATCH_BATCHES, memory_order_relaxed),
        .dispatched_logical_engines =
            atomic_load_explicit(&RUNNING_DISPATCHED_LOGICAL_ENGINES, memory_order_relaxed),
        .dispatched_chip_engines =
            atomic_load_explicit(&RUNNING_DISPATCHED_CHIP_ENGINES, memory_order_relaxed),
        .dispatch_failures = atomic_load_explicit(&RUNNING_DISPATCH_FAILURES, memory_order_relaxed),
        .mapped_results = atomic_load_explicit(&RUNNING_MAPPED_RESULTS, memory_order_relaxed),
        .mapping_rejections = atomic_load_explicit(&RUNNING_MAPPING_REJECTIONS, memory_order_relaxed),
        .mapping_rejection_streak = atomic_load_explicit(
            &RUNNING_MAPPING_REJECTION_STREAK, memory_order_seq_cst),
        .mapping_recovery_pending = atomic_load_explicit(
            &RUNNING_MAPPING_RECOVERY_PENDING, memory_order_seq_cst),
        .locally_valid_results =
            atomic_load_explicit(&RUNNING_LOCALLY_VALID_RESULTS, memory_order_relaxed),
        .locally_rejected_results =
            atomic_load_explicit(&RUNNING_LOCALLY_REJECTED_RESULTS, memory_order_relaxed),
        .duplicate_results =
            atomic_load_explicit(&RUNNING_DUPLICATE_RESULTS, memory_order_relaxed),
        .local_rejection_streak = atomic_load_explicit(
            &RUNNING_LOCAL_REJECTION_STREAK, memory_order_seq_cst),
        .local_recovery_pending = atomic_load_explicit(
            &RUNNING_LOCAL_RECOVERY_PENDING, memory_order_seq_cst),
    };
    return true;
}

static void BZM_running_record_proof(void)
{
    atomic_fetch_add_explicit(&RUNNING_LOCALLY_VALID_RESULTS, 1,
                              memory_order_seq_cst);
    atomic_store_explicit(&RUNNING_MAPPING_RECOVERY_PENDING, false,
                          memory_order_seq_cst);
    atomic_store_explicit(&RUNNING_MAPPING_REJECTION_STREAK, 0,
                          memory_order_seq_cst);
    atomic_store_explicit(&RUNNING_LOCAL_RECOVERY_PENDING, false,
                          memory_order_seq_cst);
    atomic_store_explicit(&RUNNING_LOCAL_REJECTION_STREAK, 0,
                          memory_order_seq_cst);
}

static void BZM_running_record_rejection(void)
{
    atomic_store_explicit(&RUNNING_LOCAL_RECOVERY_PENDING, true,
                          memory_order_seq_cst);
    atomic_fetch_add_explicit(&RUNNING_LOCALLY_REJECTED_RESULTS, 1,
                              memory_order_seq_cst);
    atomic_fetch_add_explicit(&RUNNING_LOCAL_REJECTION_STREAK, 1,
                              memory_order_seq_cst);
}

bool BZM_hashrate_counter_snapshot(GlobalState *state,
                                   uint32_t *difficulty_one_counters,
                                   size_t counter_count)
{
    if (BZM_STATE == NULL || state == NULL || difficulty_one_counters == NULL ||
        counter_count < state->DEVICE_CONFIG.family.asic_count ||
        state->DEVICE_CONFIG.family.asic_count > BZM_MAX_ASIC_COUNT) {
        return false;
    }
    for (size_t i = 0; i < state->DEVICE_CONFIG.family.asic_count; ++i) {
        difficulty_one_counters[i] = (uint32_t)atomic_load_explicit(
            &HASHRATE_DIFFICULTY_ONE_COUNTERS[i], memory_order_relaxed);
    }
    return true;
}

static void BZM_record_local_result(uint8_t asic_index, bool valid,
                                    double nonce_difficulty)
{
    const uint64_t difficulty_one_units =
        BZM_RESULT_DIFFICULTY;
    const bool proof =
        asic_index < BZM_MAX_ASIC_COUNT && valid &&
        bzm_running_result_meets_proof(nonce_difficulty,
            (double)BZM_RESULT_DIFFICULTY);
    if (proof) {
        /* One result passing the programmed leading-zero filter represents
         * 2^(lead_zeros - 32) difficulty-one hash counters. This feeds the
         * normal ESP-Miner/AxeOS hashrate monitor without trusting corrupted
         * or merely mapped ASIC frames. */
        atomic_fetch_add_explicit(
            &HASHRATE_DIFFICULTY_ONE_COUNTERS[asic_index],
            difficulty_one_units, memory_order_relaxed);
        BZM_running_record_proof();
    } else {
        BZM_running_record_rejection();
    }
}

bool BZM_work_replacement_snapshot(uint32_t *generation, uint32_t *completed, bool *pending)
{
    if (generation == NULL || completed == NULL || pending == NULL) return false;
    pthread_mutex_lock(&REACTOR_LOCK);
    *pending = INITIALIZED && FAST_DISPATCH_REMAINING != 0;
    *generation = (uint32_t)atomic_load_explicit(
        &WORK_REPLACEMENT_GENERATION, memory_order_relaxed);
    *completed = BZM_STATE->work_replacement_completed;
    pthread_mutex_unlock(&REACTOR_LOCK);
    return true;
}

float BZM_read_temperature(GlobalState * state)
{
    (void) state;
    if (BZM_STATE == NULL || !INITIALIZED || TRANSPORT.asic_count == 0)
        return -1.0f;

    int64_t now = esp_timer_get_time();
    if (LAST_TEMPERATURE_US != 0 && now - LAST_TEMPERATURE_US < 2000000) {
        return LAST_TEMPERATURE;
    }

    bzm_telemetry_store_t snapshot;
    pthread_mutex_lock(&REACTOR_LOCK);
    const bool available =
        bzm_serial_get_telemetry_snapshot(&TRANSPORT, &snapshot);
    pthread_mutex_unlock(&REACTOR_LOCK);

    float hottest_c = -1.0f;
    if (available &&
        bzm_telemetry_max_temperature(
            &snapshot, (uint64_t)now,
            BZM_TELEMETRY_MAX_AGE_US,
            &hottest_c)) {
        LAST_TEMPERATURE = hottest_c;
    }
    LAST_TEMPERATURE_US = now;
    return LAST_TEMPERATURE;
}

bool BZM_get_telemetry_snapshot(bzm_telemetry_store_t * snapshot)
{
    return bzm_serial_get_telemetry_snapshot(&TRANSPORT, snapshot);
}

bool BZM_get_parser_stats(bzm_serial_parser_stats_t * stats)
{
    return bzm_serial_get_parser_stats(&TRANSPORT, stats);
}

bool BZM_staged_get_parser_baseline(bzm_serial_parser_stats_t * stats)
{
    pthread_mutex_lock(&REACTOR_LOCK);
    bool available = STAGED_TRANSPORT_READY &&
                     bzm_balanced_ramp_get_parser_baseline(&STAGED_BALANCED_RAMP, stats);
    pthread_mutex_unlock(&REACTOR_LOCK);
    return available;
}

bool BZM_staged_get_sensor_parser_baseline(bzm_serial_parser_stats_t * stats)
{
    if (stats == NULL) {
        return false;
    }
    pthread_mutex_lock(&REACTOR_LOCK);
    bool available = STAGED_TRANSPORT_READY && STAGED_SENSOR_PARSER_BASELINE_VALID;
    if (available) {
        *stats = STAGED_SENSOR_PARSER_BASELINE;
    }
    pthread_mutex_unlock(&REACTOR_LOCK);
    return available;
}

bool BZM_staged_get_running_parser_baseline(bzm_serial_parser_stats_t * stats)
{
    if (stats == NULL) {
        return false;
    }
    pthread_mutex_lock(&REACTOR_LOCK);
    bool available = STAGED_TRANSPORT_READY && STAGED_RUNNING_PARSER_BASELINE_VALID;
    if (available) {
        *stats = STAGED_RUNNING_PARSER_BASELINE;
    }
    pthread_mutex_unlock(&REACTOR_LOCK);
    return available;
}

static bool staged_lease_renew(void * context)
{
    (void) context;
    if (STAGED_OPERATION_AUTHORIZE == NULL || !STAGED_OPERATION_AUTHORIZE(STAGED_OPERATION_AUTHORIZE_CONTEXT)) {
        return false;
    }
    bzm_bridge_safety_status_t status;
    bool renewed = BZM_bridge_safety_heartbeat(&status) == ESP_OK && bzm_lease_guard_status_is_controlled(&status);
    if (renewed) {
        STAGED_LEASE_SCHEDULE.renewed = true;
        STAGED_LEASE_SCHEDULE.last_renewal_ms = (uint64_t) (esp_timer_get_time() / 1000);
    }
    return renewed;
}

static bool staged_lease_check(void)
{
    if (!STAGED_LEASE_IO_OK || !staged_lease_renew(NULL)) {
        STAGED_LEASE_IO_OK = false;
        return false;
    }
    return true;
}

static bool staged_mining_lease_renew(void * context)
{
    (void) context;
    bzm_bridge_safety_status_t status;
    bool renewed = BZM_bridge_safety_heartbeat(&status) == ESP_OK && bzm_lease_guard_status_is_controlled(&status);
    if (renewed) {
        STAGED_LEASE_SCHEDULE.renewed = true;
        STAGED_LEASE_SCHEDULE.last_renewal_ms = (uint64_t) (esp_timer_get_time() / 1000);
    }
    return renewed;
}

static bool staged_mining_lease_service_due(void)
{
    uint64_t current_ms = (uint64_t) (esp_timer_get_time() / 1000);
    bool dispatch_authorized = STAGED_TRANSPORT_READY && STAGED_BRINGUP.running_verified &&
                               bzm_dispatch_gate_is_authorized(&STAGED_DISPATCH_GATE);
    if (!STAGED_LEASE_IO_OK ||
        !bzm_lease_guard_service_authorized(&STAGED_LEASE_SCHEDULE, dispatch_authorized, current_ms,
                                            BZM_LEASE_GUARD_MAX_DELAY_CHUNK_MS, staged_mining_lease_renew, NULL)) {
        STAGED_LEASE_IO_OK = false;
        return false;
    }
    return true;
}

static bool staged_live_mining_check(void)
{
    if (!STAGED_LEASE_IO_OK || !STAGED_TRANSPORT_READY ||
        !STAGED_BRINGUP.running_verified ||
        !bzm_dispatch_gate_is_authorized(&STAGED_DISPATCH_GATE) ||
        !staged_mining_lease_service_due()) {
        STAGED_LEASE_IO_OK = false;
        return false;
    }
    return true;
}

static bool staged_live_write_u32(void *context, uint8_t asic_id,
                                  uint16_t engine_id, uint8_t offset,
                                  uint32_t value)
{
    if (!staged_live_mining_check()) return false;
    uint8_t bytes[4] = {
        value & 0xff,
        (value >> 8) & 0xff,
        (value >> 16) & 0xff,
        (value >> 24) & 0xff,
    };
    return bzm_serial_write_register_to(
        context, asic_id, engine_id, offset, bytes, sizeof(bytes));
}

static bool staged_live_read_u32(void *context, uint8_t asic_id,
                                 uint16_t engine_id, uint8_t offset,
                                 uint32_t *value)
{
    if (!staged_live_mining_check() || value == NULL) return false;
    uint8_t bytes[4];
    if (!bzm_serial_read_register(
            context, asic_id, engine_id, offset, bytes, sizeof(bytes))) {
        return false;
    }
    *value = (uint32_t)bytes[0] | ((uint32_t)bytes[1] << 8) |
             ((uint32_t)bytes[2] << 16) | ((uint32_t)bytes[3] << 24);
    return true;
}

static void staged_live_delay_ms(void *context, uint32_t delay_ms)
{
    (void)context;
    while (delay_ms != 0) {
        if (!staged_live_mining_check()) return;
        const uint32_t chunk =
            delay_ms > BZM_LEASE_GUARD_MAX_DELAY_CHUNK_MS
                ? BZM_LEASE_GUARD_MAX_DELAY_CHUNK_MS
                : delay_ms;
        staged_sleep(NULL, chunk);
        delay_ms -= chunk;
    }
}

static bool staged_operation_check(void)
{
    if (!STAGED_LEASE_IO_OK || STAGED_OPERATION_AUTHORIZE == NULL ||
        !STAGED_OPERATION_AUTHORIZE(STAGED_OPERATION_AUTHORIZE_CONTEXT)) {
        STAGED_LEASE_IO_OK = false;
        return false;
    }
    return true;
}

static void staged_sleep(void * context, uint32_t delay_ms)
{
    (void) context;
    vTaskDelay(pdMS_TO_TICKS(delay_ms));
}

static bzm_bringup_probe_result_t staged_probe_noop(void * context, uint8_t asic_id)
{
    if (!staged_lease_check()) {
        return BZM_BRINGUP_PROBE_IO_ERROR;
    }
    bzm_serial_probe_result_t result = bzm_serial_probe_noop(context, asic_id);
    switch (result) {
    case BZM_SERIAL_PROBE_RESPONSE:
        return BZM_BRINGUP_PROBE_RESPONSE;
    case BZM_SERIAL_PROBE_NO_RESPONSE:
        return BZM_BRINGUP_PROBE_NO_RESPONSE;
    default:
        return BZM_BRINGUP_PROBE_IO_ERROR;
    }
}

static bool staged_write_u32(void * context, uint8_t asic_id, uint16_t engine_id, uint8_t offset, uint32_t value)
{
    if (!staged_lease_check()) {
        return false;
    }
    bzm_serial_transport_t * transport = context;
    uint8_t bytes[4] = {
        value & 0xff,
        (value >> 8) & 0xff,
        (value >> 16) & 0xff,
        (value >> 24) & 0xff,
    };
    return bzm_serial_write_register_to(transport, asic_id, engine_id, offset, bytes, sizeof(bytes));
}

static bool staged_read_u32(void * context, uint8_t asic_id, uint16_t engine_id, uint8_t offset, uint32_t * value)
{
    if (!staged_lease_check()) {
        return false;
    }
    bzm_serial_transport_t * transport = context;
    uint8_t bytes[4];
    if (value == NULL || !bzm_serial_read_register(transport, asic_id, engine_id, offset, bytes, sizeof(bytes))) {
        return false;
    }
    *value = (uint32_t) bytes[0] | ((uint32_t) bytes[1] << 8) | ((uint32_t) bytes[2] << 16) | ((uint32_t) bytes[3] << 24);
    return true;
}

static void staged_delay_ms(void * context, uint32_t delay_ms)
{
    (void) context;
    if (!STAGED_LEASE_IO_OK || !bzm_lease_guard_delay(delay_ms, staged_lease_renew, staged_sleep, NULL)) {
        STAGED_LEASE_IO_OK = false;
    }
}

static uint64_t staged_now_us(void * context)
{
    (void) context;
    return (uint64_t) esp_timer_get_time();
}

static bool staged_telemetry_snapshot(void * context, bzm_telemetry_store_t * snapshot)
{
    if (!staged_lease_check()) {
        return false;
    }
    bzm_serial_transport_t * transport = context;
    /* Pump at least one receive window so stale pre-configuration evidence
     * cannot pass merely because it was already present in the store. */
    (void) bzm_serial_poll(transport, 100);
    return bzm_serial_get_telemetry_snapshot(transport, snapshot);
}

static bool staged_ramp_telemetry_snapshot(void * context, bzm_telemetry_store_t * snapshot)
{
    if (!staged_lease_check()) {
        return false;
    }
    bzm_serial_transport_t * transport = context;
    /* BIRDS waits 30 ms after an activation batch for fresh voltage data. */
    (void) bzm_serial_poll(transport, 30);
    return bzm_serial_get_telemetry_snapshot(transport, snapshot);
}

static bool staged_ramp_begin_engine(void * context, uint8_t asic_id, uint16_t engine_id)
{
    (void) context;
    (void) asic_id;
    (void) engine_id;
    return staged_operation_check();
}

static bool staged_ramp_write_control_u32(void * context, uint8_t asic_id, uint8_t offset, uint32_t value)
{
    uint8_t bytes[4] = {
        value & 0xff,
        (value >> 8) & 0xff,
        (value >> 16) & 0xff,
        (value >> 24) & 0xff,
    };
    return staged_operation_check() &&
           bzm_serial_write_register_to(context, asic_id, BZM_BRINGUP_CONTROL_ENGINE_ID, offset, bytes, sizeof(bytes));
}

static bool staged_ramp_read_control_u32(void * context, uint8_t asic_id, uint8_t offset, uint32_t * value)
{
    uint8_t bytes[4];
    if (value == NULL || !staged_operation_check() ||
        !bzm_serial_read_register(context, asic_id, BZM_BRINGUP_CONTROL_ENGINE_ID, offset, bytes, sizeof(bytes))) {
        return false;
    }
    *value = (uint32_t) bytes[0] | ((uint32_t) bytes[1] << 8) | ((uint32_t) bytes[2] << 16) |
             ((uint32_t) bytes[3] << 24);
    return true;
}

static bool staged_ramp_wait_tdm_idle(bzm_serial_transport_t * transport)
{
    /* ASICs occupy slots 10, 20, 30, and 40 in a 100-slot TDM cycle. Finish
     * the current response and require a one-millisecond byte-free interval
     * with no partial parser frame before changing TDM state. */
    for (uint8_t attempt = 0; attempt < 64; ++attempt) {
        size_t frames = bzm_serial_poll(transport, 1);
        bzm_serial_parser_stats_t stats;
        if (!bzm_serial_get_parser_stats(transport, &stats)) {
            return false;
        }
        if (frames == 0 && stats.buffered_bytes == 0) {
            return true;
        }
    }
    bzm_serial_parser_stats_t stats = {0};
    (void) bzm_serial_get_parser_stats(transport, &stats);
    ESP_LOGE(TAG, "Engine startup could not reach a TDM idle gap: frames=%lu discarded=%lu buffered=%u",
             (unsigned long) stats.emitted_frames, (unsigned long) stats.discarded_bytes, (unsigned) stats.buffered_bytes);
    return false;
}

static bool staged_ramp_capture_batch_telemetry(bzm_serial_transport_t * transport)
{
    if (!STAGED_RAMP_TELEMETRY_POLICY_READY) {
        return false;
    }
    STAGED_BATCH_TELEMETRY_ACTIVE = false;
    bzm_telemetry_confirmation_t confirmation;
    bzm_telemetry_confirmation_init(&confirmation);
    uint8_t attempts = STAGED_RAMP_TELEMETRY_POLICY.ch2_confirm_samples * BZM_BRINGUP_ASIC_COUNT;
    for (uint8_t attempt = 0; attempt < attempts; ++attempt) {
        if (attempt != 0) {
            (void) bzm_serial_poll(transport, 30);
        }
        if (!staged_ramp_wait_tdm_idle(transport)) {
            return false;
        }

        bzm_telemetry_store_t snapshot;
        if (!bzm_serial_get_telemetry_snapshot(transport, &snapshot)) {
            return false;
        }
        uint64_t now_us = (uint64_t) esp_timer_get_time();
        bool hard_fault = false;
        for (size_t index = 0; index < BZM_BRINGUP_ASIC_COUNT; ++index) {
            uint8_t asic_id = bzm_asic_wire_ids[index];
            const bzm_telemetry_sample_t * sample = bzm_telemetry_store_get(&snapshot, asic_id);
            if (bzm_telemetry_sample_has_immediate_trip(sample)) {
                hard_fault = true;
            }
        }
        uint8_t culprit = 0;
        uint8_t observed = 0;
        bzm_ch2_confirmation_result_t confirmation_result = bzm_telemetry_confirmation_observe(
            &confirmation, &snapshot, now_us, STAGED_RAMP_TELEMETRY_POLICY.max_age_us,
            &STAGED_RAMP_TELEMETRY_POLICY.bounds, true, STAGED_RAMP_TELEMETRY_POLICY.ch2_confirm_samples,
            &culprit, &observed);
        if (confirmation_result == BZM_CH2_CONFIRMATION_GOOD) {
            STAGED_BATCH_TELEMETRY = snapshot;
            STAGED_BATCH_TELEMETRY_ACTIVE = true;
            return true;
        }

        const bzm_telemetry_sample_t * culprit_sample = bzm_telemetry_store_get(&snapshot, culprit);

        if (hard_fault || confirmation_result == BZM_CH2_CONFIRMATION_CONTINUOUS ||
            confirmation_result == BZM_CH2_CONFIRMATION_INVALID) {
            ESP_LOGE(TAG,
                     "Engine startup telemetry unsafe: asic=0x%02x samples=%u/%u temp=%.1fC ch0=%.1fmV ch1=%.1fmV ch2=%.1fmV",
                     culprit, observed,
                     STAGED_RAMP_TELEMETRY_POLICY.ch2_confirm_samples,
                     culprit_sample != NULL ? culprit_sample->temperature_c : 0.0f,
                     culprit_sample != NULL ? culprit_sample->ch0_mv : 0.0f,
                     culprit_sample != NULL ? culprit_sample->ch1_mv : 0.0f,
                     culprit_sample != NULL ? culprit_sample->ch2_mv : 0.0f);
            return false;
        }
    }
    return false;
}

static bool staged_ramp_set_tdm(void * context, bool enabled)
{
    bzm_serial_transport_t * transport = context;
    uint32_t control = bzm_bringup_reference_tdm_control();
    if (!enabled) {
        control &= ~1U;
    }
    if (!staged_lease_check()) {
        return false;
    }
    if (!enabled && !staged_ramp_capture_batch_telemetry(transport)) {
        return false;
    }
    /* One broadcast reaches every ASIC inside the same TDM idle window. Four
     * sequential unacknowledged writes can otherwise leave the chain in a
     * mixed transport mode if the later write overlaps the next slot burst. */
    if (!staged_ramp_write_control_u32(transport, BZM_ALL_ASICS, BZM_LOCAL_REG_UART_TDM_CONTROL, control)) {
        return false;
    }
    if (BZM_SERIAL_wait_tx_done(100) != ESP_OK) {
        return false;
    }
    if (!enabled) {
        for (size_t index = 0; index < BZM_BRINGUP_ASIC_COUNT; ++index) {
            uint8_t asic_id = bzm_asic_wire_ids[index];
            uint32_t actual = 0;
            bool read_ok = false;
            for (uint8_t attempt = 0; attempt < 3 && !read_ok; ++attempt) {
                read_ok = staged_ramp_read_control_u32(transport, asic_id, BZM_LOCAL_REG_UART_TDM_CONTROL, &actual);
                if (!read_ok && attempt < 2) {
                    vTaskDelay(pdMS_TO_TICKS(1));
                }
            }
            if (!read_ok || actual != control) {
                ESP_LOGE(TAG,
                         "Engine startup TDM pause readback failed: asic=0x%02x expected=0x%08lx actual=0x%08lx read_ok=%u",
                         asic_id, (unsigned long) control, (unsigned long) actual, read_ok);
                return false;
            }
        }
    }
    return staged_operation_check();
}

static bool staged_balanced_batch_begin(void * context, uint16_t pair_index)
{
    (void) pair_index;
    STAGED_ENGINE_WINDOW_ACTIVE = false;
    if (!staged_ramp_set_tdm(context, false) ||
        !bzm_serial_get_parser_stats(context, &STAGED_ENGINE_WINDOW_BASELINE) ||
        STAGED_ENGINE_WINDOW_BASELINE.queued_results != 0 || STAGED_ENGINE_WINDOW_BASELINE.buffered_bytes != 0) {
        return false;
    }
    STAGED_ENGINE_WINDOW_ACTIVE = true;
    return true;
}

static bool staged_balanced_batch_end(void * context, uint16_t pair_index)
{
    (void) pair_index;
    bzm_serial_parser_stats_t current = {0};
    if (!STAGED_ENGINE_WINDOW_ACTIVE || !bzm_serial_get_parser_stats(context, &current) ||
        !bzm_balanced_ramp_parser_window_is_clean(&STAGED_ENGINE_WINDOW_BASELINE, &current)) {
        ESP_LOGE(TAG,
                 "Engine startup parser window failed: discarded=%lu/%lu unexpected=%lu/%lu dropped=%lu/%lu "
                 "rejected=%lu/%lu unmatched=%lu/%lu telemetry_decode=%lu/%lu queued=%u buffered=%u",
                 (unsigned long) current.discarded_bytes,
                 (unsigned long) STAGED_ENGINE_WINDOW_BASELINE.discarded_bytes,
                 (unsigned long) current.unexpected_register_headers,
                 (unsigned long) STAGED_ENGINE_WINDOW_BASELINE.unexpected_register_headers,
                 (unsigned long) current.dropped_results, (unsigned long) STAGED_ENGINE_WINDOW_BASELINE.dropped_results,
                 (unsigned long) current.rejected_result_frames,
                 (unsigned long) STAGED_ENGINE_WINDOW_BASELINE.rejected_result_frames,
                 (unsigned long) current.unmatched_register_frames,
                 (unsigned long) STAGED_ENGINE_WINDOW_BASELINE.unmatched_register_frames,
                 (unsigned long) current.telemetry_decode_failures,
                 (unsigned long) STAGED_ENGINE_WINDOW_BASELINE.telemetry_decode_failures,
                 (unsigned) current.queued_results, (unsigned) current.buffered_bytes);
        return false;
    }
    STAGED_ENGINE_WINDOW_ACTIVE = false;
    bool resumed = staged_ramp_set_tdm(context, true);
    STAGED_BATCH_TELEMETRY_ACTIVE = false;
    return resumed;
}

static bool staged_ramp_write_register(void * context, uint8_t asic_id, uint16_t engine_id, uint8_t offset,
                                       const void * data, size_t data_len)
{
    return staged_operation_check() &&
           bzm_serial_write_register_to(context, asic_id, engine_id, offset, data, data_len);
}

static bool staged_ramp_read_register(void * context, uint8_t asic_id, uint16_t engine_id, uint8_t offset, void * data,
                                      size_t data_len)
{
    if (!staged_operation_check()) {
        return false;
    }
    return bzm_serial_read_register(context, asic_id, engine_id, offset,
                                    data, data_len);
}

static void staged_ramp_delay_ms(void * context, uint32_t delay_ms)
{
    (void) context;
    vTaskDelay(pdMS_TO_TICKS(delay_ms));
}

static bool staged_ramp_telemetry_sample(void * context, uint8_t asic_id, bzm_telemetry_sample_t * sample)
{
    if (!staged_operation_check() || sample == NULL) {
        return false;
    }
    if (STAGED_BATCH_TELEMETRY_ACTIVE) {
        const bzm_telemetry_sample_t * cached = bzm_telemetry_store_get(&STAGED_BATCH_TELEMETRY, asic_id);
        if (cached == NULL) {
            return false;
        }
        *sample = *cached;
        return true;
    }
    return bzm_serial_get_telemetry(context, asic_id, sample);
}

static bool staged_ramp_parser_stats(void * context, bzm_serial_parser_stats_t * stats)
{
    return staged_operation_check() && bzm_serial_get_parser_stats(context, stats);
}

static bool staged_ramp_final_barrier(void * context)
{
    bzm_serial_transport_t * transport = context;
    if (!staged_lease_check() || BZM_SERIAL_wait_tx_done(1000) != ESP_OK) {
        return false;
    }
    (void) bzm_serial_poll(transport, 30);
    return staged_lease_check();
}

static const bzm_balanced_ramp_ops_t STAGED_BALANCED_RAMP_OPS = {
    .begin_engine = staged_ramp_begin_engine,
    .write_register = staged_ramp_write_register,
    .read_register = staged_ramp_read_register,
    .delay_ms = staged_ramp_delay_ms,
    .telemetry_sample = staged_ramp_telemetry_sample,
    .parser_stats = staged_ramp_parser_stats,
    .final_barrier = staged_ramp_final_barrier,
};

static bool staged_balanced_pair_commit(void * context, uint8_t asic_id, const bzm_engine_pair_t * pair)
{
    bool committed = bzm_balanced_ramp_commit_pair(&STAGED_BALANCED_RAMP, &STAGED_BALANCED_RAMP_OPS, context, asic_id, pair);
    if (!committed) {
        ESP_LOGE(TAG,
                 "Engine pair activation failed: cause=%s asic=0x%02x engine=0x%03x reg=0x%02x expected=%lu actual=%lu "
                 "pairs=%u engines=%u",
                 bzm_balanced_ramp_failure_name(STAGED_BALANCED_RAMP.failure), STAGED_BALANCED_RAMP.failure_asic_id,
                 STAGED_BALANCED_RAMP.failure_engine_id, STAGED_BALANCED_RAMP.failure_register_offset,
                 (unsigned long) STAGED_BALANCED_RAMP.failure_expected, (unsigned long) STAGED_BALANCED_RAMP.failure_actual,
                 STAGED_BALANCED_RAMP.completed_pairs, STAGED_BALANCED_RAMP.completed_engines);
    }
    return committed;
}

static bool staged_activation_barrier(void * context, size_t asic_count, size_t pairs_per_asic)
{
    bzm_serial_parser_stats_t transition_stats = {0};
    if (bzm_serial_get_parser_stats(context, &transition_stats)) {
        (void) bzm_balanced_ramp_accept_transition_discards(&STAGED_BALANCED_RAMP, &transition_stats);
    }
    bool passed = bzm_balanced_ramp_barrier(&STAGED_BALANCED_RAMP, &STAGED_BALANCED_RAMP_OPS, context, asic_count,
                                            pairs_per_asic);
    if (!passed) {
        bzm_serial_parser_stats_t current = {0};
        (void) bzm_serial_get_parser_stats(context, &current);
        const bzm_serial_parser_stats_t * baseline = &STAGED_BALANCED_RAMP.parser_baseline;
        ESP_LOGE(TAG,
                 "Engine activation barrier failed: cause=%s expected=%lu actual=%lu "
                 "discarded=%lu/%lu unexpected_register=%lu/%lu dropped=%lu/%lu rejected=%lu/%lu "
                 "unmatched=%lu/%lu telemetry_decode=%lu/%lu queued=%u buffered=%u",
                 bzm_balanced_ramp_failure_name(STAGED_BALANCED_RAMP.failure),
                 (unsigned long) STAGED_BALANCED_RAMP.failure_expected,
                 (unsigned long) STAGED_BALANCED_RAMP.failure_actual, (unsigned long) current.discarded_bytes,
                 (unsigned long) baseline->discarded_bytes, (unsigned long) current.unexpected_register_headers,
                 (unsigned long) baseline->unexpected_register_headers, (unsigned long) current.dropped_results,
                 (unsigned long) baseline->dropped_results, (unsigned long) current.rejected_result_frames,
                 (unsigned long) baseline->rejected_result_frames, (unsigned long) current.unmatched_register_frames,
                 (unsigned long) baseline->unmatched_register_frames, (unsigned long) current.telemetry_decode_failures,
                 (unsigned long) baseline->telemetry_decode_failures, (unsigned) current.queued_results,
                 (unsigned) current.buffered_bytes);
    }
    return passed;
}

static const bzm_bringup_ops_t STAGED_OPS = {
    .probe_noop = staged_probe_noop,
    .write_u32 = staged_write_u32,
    .read_u32 = staged_read_u32,
    .delay_ms = staged_delay_ms,
    .now_us = staged_now_us,
    .telemetry_snapshot = staged_telemetry_snapshot,

    .balanced_batch_begin = staged_balanced_batch_begin,
    .balanced_pair_commit = staged_balanced_pair_commit,
    .balanced_batch_end = staged_balanced_batch_end,
    .activation_barrier = staged_activation_barrier,
};

static const bzm_bringup_ops_t STAGED_RAMP_OPS = {
    .probe_noop = staged_probe_noop,
    .write_u32 = staged_write_u32,
    .read_u32 = staged_read_u32,
    .delay_ms = staged_delay_ms,
    .now_us = staged_now_us,
    .telemetry_snapshot = staged_ramp_telemetry_snapshot,
    .balanced_batch_begin = staged_balanced_batch_begin,
    .balanced_pair_commit = staged_balanced_pair_commit,
    .balanced_batch_end = staged_balanced_batch_end,
    .activation_barrier = staged_activation_barrier,
};

static const bzm_bringup_ops_t STAGED_LIVE_FREQUENCY_OPS = {
    .write_u32 = staged_live_write_u32,
    .read_u32 = staged_live_read_u32,
    .delay_ms = staged_live_delay_ms,
    .now_us = staged_now_us,
};

static bool staged_mining_write_work(void * context, const bzm_work_t * work)
{
    /* Called once per engine from the reactor while REACTOR_LOCK is held.
     * Re-checking the external lease/interlock callback here stops a long
     * multi-engine dispatch as soon as authorization is withdrawn. */
    if (!STAGED_TRANSPORT_READY || !STAGED_BRINGUP.running_verified || !bzm_dispatch_gate_is_authorized(&STAGED_DISPATCH_GATE)) {
        return false;
    }
    return bzm_serial_write_work(context, work);
}

static bool staged_mining_dispatch_checkpoint(void * context)
{
    bzm_serial_transport_t * transport = context;
    if (transport == NULL || !STAGED_TRANSPORT_READY ||
        !STAGED_BRINGUP.running_verified ||
        !bzm_dispatch_gate_is_authorized(&STAGED_DISPATCH_GATE) ||
        !staged_mining_lease_service_due() ||
        BZM_SERIAL_wait_tx_done(100) != ESP_OK) {
        return false;
    }

    /* A full 236-engine dispatch keeps the 2 Mbaud ESP-to-bridge link busy
     * for a bounded interval. The bridge is also
     * forwarding addressed TDM telemetry/results in the other direction.
     * Give that receive path a bounded idle interval, then drain the ESP RX
     * ring before programming the next logical engine. Parser integrity is
     * still enforced without tolerance by the runtime baseline. */
    esp_rom_delay_us(BZM_DISPATCH_GAP_US);
    (void) bzm_serial_poll(transport, 1);
    return bzm_dispatch_gate_is_authorized(&STAGED_DISPATCH_GATE);
}

static const bzm_transport_ops_t STAGED_MINING_OPS = {
    .write_work = staged_mining_write_work,
    .dispatch_checkpoint = staged_mining_dispatch_checkpoint,
    .flush = bzm_serial_flush,
};

static bool staged_fail_closed_locked(void)
{
    INITIALIZED = false;
    STAGED_TRANSPORT_READY = false;
    STAGED_LEASE_IO_OK = false;
    STAGED_LEASE_SCHEDULE = (bzm_lease_guard_schedule_t){0};
    TRANSPORT.asic_count = 0;
    bzm_bringup_init(&STAGED_BRINGUP);
    bzm_balanced_ramp_init(&STAGED_BALANCED_RAMP);
    STAGED_ENGINE_WINDOW_BASELINE = (bzm_serial_parser_stats_t){0};
    STAGED_ENGINE_WINDOW_ACTIVE = false;
    STAGED_SENSOR_PARSER_BASELINE = (bzm_serial_parser_stats_t){0};
    STAGED_SENSOR_PARSER_BASELINE_VALID = false;
    STAGED_RUNNING_PARSER_BASELINE = (bzm_serial_parser_stats_t){0};
    STAGED_RUNNING_PARSER_BASELINE_VALID = false;
    STAGED_RAMP_TELEMETRY_POLICY_READY = false;
    STAGED_BATCH_TELEMETRY_ACTIVE = false;
    bzm_dispatch_gate_init(&STAGED_DISPATCH_GATE);
    return BZM_bridge_set_asic_reset(false) == ESP_OK;
}

bzm_bringup_outcome_t BZM_staged_initialize(GlobalState * state, bzm_bringup_report_t * report)
{
    if (!BZM_driver_state_init(state)) {
        staged_report(report, BZM_BRINGUP_BAD,
                      BZM_BRINGUP_REASON_INVALID_ARGUMENT);
        return BZM_BRINGUP_BAD;
    }
    if (state->DEVICE_CONFIG.family.asic.core_count != BZM_ENGINES_PER_ASIC ||
        state->DEVICE_CONFIG.family.asic_count != BZM_BRINGUP_ASIC_COUNT) {
        pthread_mutex_lock(&REACTOR_LOCK);
        (void) staged_fail_closed_locked();
        pthread_mutex_unlock(&REACTOR_LOCK);
        staged_report(report, BZM_BRINGUP_BAD, BZM_BRINGUP_REASON_INVALID_ARGUMENT);
        return BZM_BRINGUP_BAD;
    }

    pthread_mutex_lock(&REACTOR_LOCK);
    INITIALIZED = false;
    STAGED_TRANSPORT_READY = false;
    STAGED_LEASE_IO_OK = true;
    STAGED_LEASE_SCHEDULE = (bzm_lease_guard_schedule_t){0};
    bzm_bringup_init(&STAGED_BRINGUP);
    bzm_balanced_ramp_init(&STAGED_BALANCED_RAMP);
    STAGED_ENGINE_WINDOW_BASELINE = (bzm_serial_parser_stats_t){0};
    STAGED_ENGINE_WINDOW_ACTIVE = false;
    STAGED_SENSOR_PARSER_BASELINE = (bzm_serial_parser_stats_t){0};
    STAGED_SENSOR_PARSER_BASELINE_VALID = false;
    STAGED_RUNNING_PARSER_BASELINE = (bzm_serial_parser_stats_t){0};
    STAGED_RUNNING_PARSER_BASELINE_VALID = false;
    STAGED_RAMP_TELEMETRY_POLICY_READY = false;
    STAGED_BATCH_TELEMETRY_ACTIVE = false;
    bzm_dispatch_gate_init(&STAGED_DISPATCH_GATE);

    bool lease_ready = staged_lease_check();
    bool reset_held = lease_ready && BZM_bridge_set_asic_reset(false) == ESP_OK;
    bool serial_ready = BZM_SERIAL_prepare_session(BZM_BAUD_RATE) == ESP_OK;
    bzm_serial_transport_deinit(&TRANSPORT);
    TRANSPORT = (bzm_serial_transport_t){
        .engine_count = BZM_ENGINES_PER_ASIC,
        .enhanced_mode = true,
    };
    bool transport_ready = bzm_serial_transport_init(&TRANSPORT);
    STAGED_TRANSPORT_READY = lease_ready && reset_held && serial_ready && transport_ready;
    if (!STAGED_TRANSPORT_READY) {
        (void) staged_fail_closed_locked();
    }
    pthread_mutex_unlock(&REACTOR_LOCK);

    if (!STAGED_TRANSPORT_READY) {
        staged_report(report, BZM_BRINGUP_BAD, BZM_BRINGUP_REASON_IO);
        return BZM_BRINGUP_BAD;
    }
    staged_report(report, BZM_BRINGUP_GOOD, BZM_BRINGUP_REASON_NONE);
    return BZM_BRINGUP_GOOD;
}

bzm_bringup_outcome_t BZM_staged_chain4(bzm_bringup_report_t * report)
{
    pthread_mutex_lock(&REACTOR_LOCK);
    if (!STAGED_TRANSPORT_READY) {
        pthread_mutex_unlock(&REACTOR_LOCK);
        staged_report(report, BZM_BRINGUP_BLOCKED, BZM_BRINGUP_REASON_PREREQUISITE);
        return BZM_BRINGUP_BLOCKED;
    }

    /* Renew as the immediately preceding bridge command. The pulse itself
     * contains only a bounded 200 ms reset-low interval. */
    if (!staged_lease_check() || BZM_bridge_pulse_asic_reset() != ESP_OK) {
        (void) staged_fail_closed_locked();
        pthread_mutex_unlock(&REACTOR_LOCK);
        staged_report(report, BZM_BRINGUP_BAD, BZM_BRINGUP_REASON_IO);
        return BZM_BRINGUP_BAD;
    }
    staged_delay_ms(&TRANSPORT, 1000);

    bzm_bringup_outcome_t outcome = bzm_bringup_stage_chain4(&STAGED_BRINGUP, &STAGED_OPS, &TRANSPORT, report);
    if (outcome == BZM_BRINGUP_GOOD) {
        TRANSPORT.asic_count = BZM_BRINGUP_ASIC_COUNT;
        for (uint8_t index = 0; index < BZM_BRINGUP_ASIC_COUNT; ++index) {
            TRANSPORT.asic_ids[index] = bzm_asic_wire_ids[index];
        }
    } else {
        (void) staged_fail_closed_locked();
    }
    pthread_mutex_unlock(&REACTOR_LOCK);
    return outcome;
}

bzm_bringup_outcome_t BZM_staged_sensors(const bzm_bringup_sensor_profile_t * profile,
                                         const bzm_bringup_telemetry_policy_t * telemetry_policy, bzm_bringup_report_t * report)
{
    pthread_mutex_lock(&REACTOR_LOCK);
    if (!STAGED_TRANSPORT_READY) {
        pthread_mutex_unlock(&REACTOR_LOCK);
        staged_report(report, BZM_BRINGUP_BLOCKED, BZM_BRINGUP_REASON_PREREQUISITE);
        return BZM_BRINGUP_BLOCKED;
    }
    STAGED_SENSOR_PARSER_BASELINE = (bzm_serial_parser_stats_t){0};
    STAGED_SENSOR_PARSER_BASELINE_VALID = false;
    bzm_serial_parser_stats_t pre_sensor_baseline = {0};
    bool baseline_ready = bzm_serial_get_parser_stats(&TRANSPORT, &pre_sensor_baseline) &&
                          pre_sensor_baseline.queued_results == 0 && pre_sensor_baseline.buffered_bytes == 0;
    bzm_bringup_outcome_t outcome = baseline_ready
        ? bzm_bringup_stage_sensors(&STAGED_BRINGUP, &STAGED_OPS, &TRANSPORT, profile, telemetry_policy, report)
        : BZM_BRINGUP_BAD;
    if (!baseline_ready) {
        staged_report(report, BZM_BRINGUP_BAD, BZM_BRINGUP_REASON_ACTIVATION_BARRIER);
    } else if (outcome == BZM_BRINGUP_GOOD &&
               !staged_settle_parser(&TRANSPORT, "telemetry startup", &pre_sensor_baseline,
                                     &STAGED_SENSOR_PARSER_BASELINE)) {
        STAGED_BRINGUP.sensors_verified = false;
        staged_report(report, BZM_BRINGUP_BAD, BZM_BRINGUP_REASON_ACTIVATION_BARRIER);
        outcome = BZM_BRINGUP_BAD;
    } else if (outcome == BZM_BRINGUP_GOOD) {
        STAGED_SENSOR_PARSER_BASELINE_VALID = true;
    }
    if (outcome != BZM_BRINGUP_GOOD)
        (void) staged_fail_closed_locked();
    pthread_mutex_unlock(&REACTOR_LOCK);
    return outcome;
}

bzm_bringup_outcome_t BZM_staged_clocks(const bzm_bringup_pll_profile_t * profile,
                                        const bzm_bringup_telemetry_policy_t * telemetry_policy, bzm_bringup_report_t * report)
{
    pthread_mutex_lock(&REACTOR_LOCK);
    if (!STAGED_TRANSPORT_READY) {
        pthread_mutex_unlock(&REACTOR_LOCK);
        staged_report(report, BZM_BRINGUP_BLOCKED, BZM_BRINGUP_REASON_PREREQUISITE);
        return BZM_BRINGUP_BLOCKED;
    }
    bzm_bringup_outcome_t outcome =
        bzm_bringup_stage_clocks(&STAGED_BRINGUP, &STAGED_OPS, &TRANSPORT, profile, telemetry_policy, report);
    if (outcome != BZM_BRINGUP_GOOD)
        (void) staged_fail_closed_locked();
    pthread_mutex_unlock(&REACTOR_LOCK);
    return outcome;
}

bzm_bringup_outcome_t BZM_staged_frequency_domains_step_live(
    const float
        target_mhz[BZM_BRINGUP_ASIC_COUNT][BZM_BRINGUP_PLL_COUNT],
    bool allow_initial_jump, bzm_bringup_report_t *report,
    float *actual_mhz)
{
    pthread_mutex_lock(&REACTOR_LOCK);
    if (!STAGED_TRANSPORT_READY || !STAGED_LEASE_IO_OK ||
        !STAGED_BRINGUP.running_verified ||
        !bzm_dispatch_gate_is_authorized(&STAGED_DISPATCH_GATE)) {
        if (actual_mhz != NULL) *actual_mhz = STAGED_BRINGUP.clock_mhz;
        pthread_mutex_unlock(&REACTOR_LOCK);
        staged_report(report, BZM_BRINGUP_BLOCKED,
                      BZM_BRINGUP_REASON_PREREQUISITE);
        return BZM_BRINGUP_BLOCKED;
    }

    bzm_bringup_outcome_t outcome =
        bzm_bringup_live_frequency_domains_step(
            &STAGED_BRINGUP, &STAGED_LIVE_FREQUENCY_OPS, &TRANSPORT,
            target_mhz, allow_initial_jump, report);
    if (outcome == BZM_BRINGUP_GOOD) {
        /* Keep mining live; replace work that crossed the PLL transition. */
        bzm_reactor_invalidate_work(&REACTOR);
        bzm_serial_discard_pending_results(&TRANSPORT);
        // A PLL transition also preserves the logical pool-job identity.
        start_fast_dispatch_locked(REACTOR.config.engine_count);
    }
    if (actual_mhz != NULL) *actual_mhz = STAGED_BRINGUP.clock_mhz;
    pthread_mutex_unlock(&REACTOR_LOCK);
    return outcome;
}

bzm_bringup_outcome_t BZM_staged_balanced_ramp(const bzm_bringup_telemetry_policy_t * telemetry_policy,
                                               bzm_bringup_report_t * report)
{
    pthread_mutex_lock(&REACTOR_LOCK);
    if (!STAGED_TRANSPORT_READY) {
        pthread_mutex_unlock(&REACTOR_LOCK);
        staged_report(report, BZM_BRINGUP_BLOCKED, BZM_BRINGUP_REASON_PREREQUISITE);
        return BZM_BRINGUP_BLOCKED;
    }
    STAGED_RAMP_TELEMETRY_POLICY_READY = telemetry_policy != NULL;
    if (telemetry_policy != NULL) {
        STAGED_RAMP_TELEMETRY_POLICY = *telemetry_policy;
    }
    STAGED_BATCH_TELEMETRY_ACTIVE = false;
    bzm_bringup_outcome_t outcome =
        bzm_bringup_stage_balanced_ramp(&STAGED_BRINGUP, &STAGED_RAMP_OPS, &TRANSPORT, telemetry_policy, report);
    STAGED_RAMP_TELEMETRY_POLICY_READY = false;
    STAGED_BATCH_TELEMETRY_ACTIVE = false;
    if (outcome != BZM_BRINGUP_GOOD && report != NULL && STAGED_BALANCED_RAMP.failure != BZM_BALANCED_RAMP_FAILURE_NONE) {
        report->asic_id = STAGED_BALANCED_RAMP.failure_asic_id;
        report->register_offset = STAGED_BALANCED_RAMP.failure_register_offset;
        report->expected = STAGED_BALANCED_RAMP.failure_expected;
        report->actual = STAGED_BALANCED_RAMP.failure_actual;
    }
    if (outcome != BZM_BRINGUP_GOOD)
        (void) staged_fail_closed_locked();
    pthread_mutex_unlock(&REACTOR_LOCK);
    return outcome;
}

static bool staged_poll_parser_settle_window(bzm_serial_transport_t * transport, uint32_t window_ms,
                                             bzm_serial_parser_stats_t * stats)
{
    int64_t deadline_us = esp_timer_get_time() + (int64_t) window_ms * 1000;
    while (esp_timer_get_time() < deadline_us) {
        int64_t remaining_us = deadline_us - esp_timer_get_time();
        uint16_t timeout_ms = (uint16_t) ((remaining_us + 999) / 1000);
        if (timeout_ms > 10) {
            timeout_ms = 10;
        }
        if (timeout_ms == 0) {
            timeout_ms = 1;
        }
        (void) bzm_serial_poll(transport, timeout_ms);
    }

    /* End at a parser frame boundary. Valid frames may arrive in every poll
     * with four spaced TDM senders, so requiring a frame-free millisecond can
     * falsely time out even though no partial frame remains. */
    for (uint8_t attempt = 0; attempt < 64; ++attempt) {
        (void) bzm_serial_poll(transport, 1);
        if (!bzm_serial_get_parser_stats(transport, stats)) {
            return false;
        }
        if (bzm_parser_settling_snapshot_at_frame_boundary(stats)) {
            return true;
        }
    }
    return false;
}

static bool staged_settle_parser(bzm_serial_transport_t * transport, const char * stage_name,
                                 const bzm_serial_parser_stats_t * initial_baseline,
                                 bzm_serial_parser_stats_t * accepted_baseline)
{
    if (transport == NULL || stage_name == NULL || initial_baseline == NULL || accepted_baseline == NULL) {
        return false;
    }

    bzm_parser_settling_t settling;
    bzm_parser_settling_init(&settling, initial_baseline);
    bzm_serial_parser_stats_t current = *initial_baseline;
    bzm_parser_settling_result_t result = BZM_PARSER_SETTLING_PENDING;
    for (uint8_t window = 0; result == BZM_PARSER_SETTLING_PENDING && window < BZM_PARSER_SETTLE_MAX_WINDOWS; ++window) {
        if (!staged_lease_check() ||
            !staged_poll_parser_settle_window(transport, BZM_PARSER_SETTLE_WINDOW_MS, &current)) {
            result = BZM_PARSER_SETTLING_BAD;
            break;
        }
        uint32_t discarded_before = settling.baseline.discarded_bytes;
        result = bzm_parser_settling_observe(&settling, &current, BZM_PARSER_SETTLE_REQUIRED_CLEAN_WINDOWS);
        if (result != BZM_PARSER_SETTLING_BAD && current.discarded_bytes != discarded_before) {
            ESP_LOGW(TAG,
                     "%s settling accepted transition discard burst: baseline=%lu current=%lu; next stage remains closed",
                     stage_name, (unsigned long) discarded_before, (unsigned long) current.discarded_bytes);
        }
    }

    if (result != BZM_PARSER_SETTLING_COMPLETE) {
        ESP_LOGE(TAG,
                 "%s parser did not settle: cleanWindows=%u/%u discarded=%lu unexpected=%lu dropped=%lu "
                 "rejected=%lu unmatched=%lu telemetryDecode=%lu queued=%u buffered=%u",
                 stage_name, settling.clean_windows, BZM_PARSER_SETTLE_REQUIRED_CLEAN_WINDOWS,
                 (unsigned long) current.discarded_bytes, (unsigned long) current.unexpected_register_headers,
                 (unsigned long) current.dropped_results, (unsigned long) current.rejected_result_frames,
                 (unsigned long) current.unmatched_register_frames, (unsigned long) current.telemetry_decode_failures,
                 (unsigned) current.queued_results, (unsigned) current.buffered_bytes);
        if (current.recent_discarded_length != 0) {
            ESP_LOG_BUFFER_HEX_LEVEL(TAG, current.recent_discarded_bytes,
                                     current.recent_discarded_length, ESP_LOG_ERROR);
        }
        return false;
    }

    *accepted_baseline = settling.baseline;
    return true;
}

bzm_bringup_outcome_t BZM_staged_running(GlobalState * state, const bzm_bringup_telemetry_policy_t * telemetry_policy,
                                         bzm_bringup_report_t * report)
{
    pthread_mutex_lock(&REACTOR_LOCK);
    if (!STAGED_TRANSPORT_READY) {
        pthread_mutex_unlock(&REACTOR_LOCK);
        staged_report(report, BZM_BRINGUP_BLOCKED, BZM_BRINGUP_REASON_PREREQUISITE);
        return BZM_BRINGUP_BLOCKED;
    }
    if (state == NULL) {
        (void) staged_fail_closed_locked();
        pthread_mutex_unlock(&REACTOR_LOCK);
        staged_report(report, BZM_BRINGUP_BAD, BZM_BRINGUP_REASON_INVALID_ARGUMENT);
        return BZM_BRINGUP_BAD;
    }
    STAGED_RUNNING_PARSER_BASELINE = (bzm_serial_parser_stats_t){0};
    STAGED_RUNNING_PARSER_BASELINE_VALID = false;
    bzm_bringup_outcome_t outcome = bzm_bringup_check_running(&STAGED_BRINGUP, &STAGED_OPS, &TRANSPORT, telemetry_policy, report);
    if (outcome == BZM_BRINGUP_GOOD) {
        bzm_reactor_config_t config = {
            .engine_count = BZM_ENGINES_PER_ASIC,
            /* Match the BIRDS production work budget after the verified
             * bring-up path rebuilds the reactor for mining. */
            .timestamp_count = 60,
            .lead_zeros = BZM_RESULT_LEAD_ZEROS,
            .nonce_offset = BZM_NONCE_GAP_1002,
            .enhanced_mode = true,
        };
        INITIALIZED = bzm_reactor_init(&REACTOR, &BZM_STATE->job_store, &config, &STAGED_MINING_OPS, &TRANSPORT);
        /* A pool's clean-job notification can arrive before board startup.
         * Prime every engine at the fast cadence after rebuilding the reactor,
         * including on resume, so the first rotation meets the proof deadline. */
        start_fast_dispatch_locked(INITIALIZED ? config.engine_count : 0);
        if (!INITIALIZED) {
            STAGED_BRINGUP.running_verified = false;
            staged_report(report, BZM_BRINGUP_BAD, BZM_BRINGUP_REASON_IO);
            outcome = BZM_BRINGUP_BAD;
        }
        if (outcome == BZM_BRINGUP_GOOD) {
            bzm_serial_parser_stats_t pre_running_baseline = {0};
            bool baseline_ready = bzm_serial_get_parser_stats(&TRANSPORT, &pre_running_baseline) &&
                                  pre_running_baseline.queued_results == 0 &&
                                  pre_running_baseline.buffered_bytes == 0;
            if (!baseline_ready ||
                !staged_settle_parser(&TRANSPORT, "mining startup", &pre_running_baseline,
                                      &STAGED_RUNNING_PARSER_BASELINE)) {
                STAGED_BRINGUP.running_verified = false;
                staged_report(report, BZM_BRINGUP_BAD, BZM_BRINGUP_REASON_ACTIVATION_BARRIER);
                outcome = BZM_BRINGUP_BAD;
            } else {
                STAGED_RUNNING_PARSER_BASELINE_VALID = true;
            }
        }
    }
    if (outcome != BZM_BRINGUP_GOOD)
        (void) staged_fail_closed_locked();
    pthread_mutex_unlock(&REACTOR_LOCK);
    return outcome;
}

bool BZM_staged_get_state(bzm_bringup_state_t * state)
{
    if (state == NULL)
        return false;
    pthread_mutex_lock(&REACTOR_LOCK);
    *state = STAGED_BRINGUP;
    pthread_mutex_unlock(&REACTOR_LOCK);
    return true;
}

bool BZM_staged_hold_reset(void)
{
    pthread_mutex_lock(&REACTOR_LOCK);
    bool held = staged_fail_closed_locked();
    pthread_mutex_unlock(&REACTOR_LOCK);
    return held;
}

void BZM_staged_set_dispatch_authorizer(bzm_dispatch_authorizer_t authorize, void * context)
{
    pthread_mutex_lock(&REACTOR_LOCK);
    bzm_dispatch_gate_set(&STAGED_DISPATCH_GATE, authorize, context);
    pthread_mutex_unlock(&REACTOR_LOCK);
}

void BZM_staged_set_operation_authorizer(bzm_dispatch_authorizer_t authorize, void * context)
{
    pthread_mutex_lock(&REACTOR_LOCK);
    STAGED_OPERATION_AUTHORIZE = authorize;
    STAGED_OPERATION_AUTHORIZE_CONTEXT = context;
    pthread_mutex_unlock(&REACTOR_LOCK);
}

size_t BZM_staged_poll(uint16_t timeout_ms)
{
    if (timeout_ms == 0)
        return 0;
    pthread_mutex_lock(&REACTOR_LOCK);
    size_t frames = STAGED_TRANSPORT_READY ? bzm_serial_poll(&TRANSPORT, timeout_ms) : 0;
    pthread_mutex_unlock(&REACTOR_LOCK);
    return frames;
}

/* Translate only at the driver boundary. PR #1972 already carries an owned
 * common job: resolving its ntime here needs no extra shared result fields. */
task_result *BZM_process_work(GlobalState *state)
{
    const bzm_result_t *share = BZM_process_result(state);
    if (share == NULL) return NULL;
    static task_result result;
    if (!bzm_result_to_task(share, &result) ||
        !bzm_job_store_contains(&BZM_STATE->job_store, share->work_handle)) return NULL;
    double difficulty = mining_nonce_difficulty(&result.job, result.nonce,
                                                result.rolled_version);
    bool valid = difficulty >= BZM_RESULT_DIFFICULTY;
    BZM_record_local_result(share->asic_index, valid, difficulty);
    return valid ? &result : NULL;
}

void BZM_submit_job(GlobalState *state, const asic_job_t *job)
{
    if (BZM_STATE == NULL || state == NULL || job == NULL) return;
    unsigned epoch = atomic_load_explicit(&BZM_STATE->work_epoch, memory_order_acquire);
    while (state->ASIC_initalized && !state->SYSTEM_MODULE.mining_paused &&
           !state->SYSTEM_MODULE.hardware_fault &&
           !state->SYSTEM_MODULE.pools_unavailable &&
           epoch == atomic_load_explicit(&BZM_STATE->work_epoch, memory_order_acquire)) {
        if (BZM_send_work(state, job)) return;
        vTaskDelay(pdMS_TO_TICKS(20));
    }
}
