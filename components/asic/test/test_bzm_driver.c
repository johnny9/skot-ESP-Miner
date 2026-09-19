#include "bzm_test_state.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#define BZM_driver_state_init test_driver_state_init
#define BZM_set_max_baud test_driver_set_max_baud
#define BZM_submit_job test_driver_submit_job
#define BZM_clear_work test_driver_clear_work
#define BZM_job_frequency_ms test_driver_job_frequency_ms
#define BZM_process_work test_driver_process_work
#define BZM_read_temperature test_driver_read_temperature
#define BZM_hashrate_counter_snapshot test_driver_hashrate_snapshot
#define BZM_has_io_fault test_driver_has_io_fault
#define BZM_work_replacement_snapshot test_driver_replacement_snapshot
#define BZM_get_telemetry_snapshot test_driver_telemetry_snapshot
#define BZM_initialize_transport test_driver_initialize_transport
#define BZM_discover_chain test_driver_discover_chain
#define BZM_configure_sensors test_driver_configure_sensors
#define BZM_configure_clocks test_driver_configure_clocks
#define BZM_step_frequency_domains test_driver_step_frequency_domains
#define BZM_activate_engines test_driver_activate_engines
#define BZM_start_mining test_driver_start_mining
#define BZM_get_state test_driver_get_state
#define BZM_hold_reset test_driver_hold_reset
#define BZM_set_dispatch_authorizer test_driver_dispatch_authorizer
#define BZM_set_operation_authorizer test_driver_operation_authorizer
#define BZM_poll test_driver_poll
#define bzm_serial_read_result test_driver_read_result
#define mining_nonce_difficulty test_driver_difficulty

#include "../bzm_driver.c"
#include "unity.h"

static bool authorized, fail_write, cancel_write, fail_flush;
static double result_difficulty;
static bzm_work_t last_work;
static bzm_raw_result_t raw_result;
static GlobalState test_state;

static bool allow_dispatch(void *context) { (void)context; return authorized; }
static bool write_work(void *context, const bzm_work_t *work)
{
    (void)context;
    last_work = *work;
    if (cancel_write) authorized = false;
    return !fail_write && !cancel_write;
}
static bool flush(void *context) { (void)context; return !fail_flush; }
bool test_driver_read_result(bzm_serial_transport_t *transport, bzm_raw_result_t *result, uint16_t timeout_ms)
{
    (void)transport; (void)timeout_ms;
    *result = raw_result;
    return true;
}
double test_driver_difficulty(const asic_job_t *job, uint32_t nonce, uint32_t version)
{
    (void)job; (void)nonce; (void)version;
    return result_difficulty;
}

static void fixture_init(void)
{
    BZM_STATE = calloc(1, sizeof(*BZM_STATE));
    TEST_ASSERT_NOT_NULL(BZM_STATE);
    TEST_ASSERT_TRUE(bzm_job_store_init(&BZM_STATE->job_store));
    TEST_ASSERT_EQUAL_INT(0, pthread_mutex_init(&REACTOR_LOCK, NULL));
    atomic_init(&BZM_STATE->work_epoch, 0);
    atomic_init(&BZM_STATE->io_failed, false);
    atomic_init(&WORK_REPLACEMENT_GENERATION, 0);
    for (size_t i = 0; i < BZM_MAX_ASIC_COUNT; ++i) atomic_init(&HASHRATE_DIFFICULTY_ONE_COUNTERS[i], 0);
    bzm_reactor_config_t config = {.engine_count = BZM_ENGINES_PER_ASIC, .timestamp_count = 16,
                                   .enhanced_mode = true, .nonce_offset = BZM_NONCE_GAP_1002};
    bzm_transport_ops_t ops = {.write_work = write_work, .flush = flush};
    TEST_ASSERT_TRUE(bzm_reactor_init(&REACTOR, &BZM_STATE->job_store, &config, &ops, NULL));
    INITIALIZED = DRIVER_TRANSPORT_READY = DRIVER_BRINGUP.running = true;
    BZM_set_dispatch_authorizer(allow_dispatch, NULL);
    authorized = true;
    fail_write = cancel_write = fail_flush = false;
    memset(&test_state, 0, sizeof(test_state));
    test_state.DEVICE_CONFIG.family = FAMILY_BONANZA;
    test_state.ASIC_initalized = true;
}
static void fixture_destroy(void)
{
    bzm_job_store_destroy(&BZM_STATE->job_store);
    TEST_ASSERT_EQUAL_INT(0, pthread_mutex_destroy(&REACTOR_LOCK));
    free(BZM_STATE);
    BZM_STATE = NULL;
}
static asic_job_t template(void)
{
    return (asic_job_t){.version = 0x20000000, .ntime = 1234, .nbits = 0x1702369d,
                        .version_mask = 0x6000, .pool_diff = 1};
}

TEST_CASE("BZM clean jobs restart a complete fast replacement rotation", "[asic][bzm][driver]")
{
    fixture_init();
    asic_job_t job = template();
    TEST_ASSERT_TRUE(BZM_clear_work(&test_state));
    for (size_t i = 0; i < BZM_ENGINES_PER_ASIC - 1; ++i) TEST_ASSERT_TRUE(BZM_send_work(&test_state, &job));
    TEST_ASSERT_TRUE(BZM_clear_work(&test_state));
    uint32_t generation, completed;
    bool pending;
    for (size_t i = 0; i < BZM_ENGINES_PER_ASIC - 1; ++i) {
        TEST_ASSERT_TRUE(BZM_send_work(&test_state, &job));
        TEST_ASSERT_EQUAL_DOUBLE(10.0, BZM_job_frequency_ms(&test_state));
    }
    TEST_ASSERT_TRUE(BZM_send_work(&test_state, &job));
    TEST_ASSERT_TRUE(BZM_work_replacement_snapshot(&generation, &completed, &pending));
    TEST_ASSERT_FALSE(pending);
    TEST_ASSERT_EQUAL_UINT32(generation, completed);
    TEST_ASSERT_EQUAL_DOUBLE(100.0, BZM_job_frequency_ms(&test_state));
    fixture_destroy();
}

TEST_CASE("BZM frequent clean jobs cannot starve clock-change synchronization", "[asic][bzm][driver]")
{
    fixture_init();
    asic_job_t job = template();
    start_work_replacement_locked(BZM_ENGINES_PER_ASIC);
    uint32_t generation, completed;
    bool pending;
    for (size_t i = 0; i < BZM_ENGINES_PER_ASIC; ++i) {
        if (i % 25 == 0) TEST_ASSERT_TRUE(BZM_clear_work(&test_state));
        TEST_ASSERT_TRUE(BZM_work_replacement_snapshot(&generation, &completed, &pending));
        TEST_ASSERT_TRUE(pending);
        TEST_ASSERT_TRUE(BZM_send_work(&test_state, &job));
    }
    TEST_ASSERT_TRUE(BZM_work_replacement_snapshot(&generation, &completed, &pending));
    TEST_ASSERT_FALSE(pending);
    TEST_ASSERT_EQUAL_UINT32(1, generation);
    TEST_ASSERT_EQUAL_UINT32(generation, completed);
    TEST_ASSERT_EQUAL_DOUBLE(10.0, BZM_job_frequency_ms(&test_state));
    fixture_destroy();
}

TEST_CASE("BZM real transport errors latch but withdrawn dispatch does not", "[asic][bzm][driver]")
{
    for (unsigned failure = 0; failure < 3; ++failure) {
        fixture_init();
        asic_job_t job = template();
        fail_write = failure == 0;
        cancel_write = failure == 1;
        if (failure == 2) {
            REACTOR.next_engine_sequence[0] = 63;
            fail_flush = true;
        }
        TEST_ASSERT_FALSE(BZM_send_work(&test_state, &job));
        TEST_ASSERT_EQUAL(failure != 1, BZM_has_io_fault());
        fixture_destroy();
    }
}

TEST_CASE("BZM difficulty filtering rejects nonfinite results and credits only valid hashes", "[asic][bzm][driver]")
{
    fixture_init();
    asic_job_t job = template();
    TEST_ASSERT_TRUE(BZM_send_work(&test_state, &job));
    raw_result = (bzm_raw_result_t){.asic_id = BZM_FIRST_ASIC_ID, .engine_id = last_work.engine_id,
                                   .status = 8, .time = 16, .sequence_id = 0};
    const double cases[] = {NAN, INFINITY, -INFINITY, 0.0, 15.99, 16.0, 32.0};
    uint32_t counters[BZM_MAX_ASIC_COUNT] = {0};
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); ++i) {
        result_difficulty = cases[i];
        ++raw_result.nonce;
        task_result *result = BZM_process_work(&test_state);
        TEST_ASSERT_EQUAL(i >= 5, result != NULL);
    }
    TEST_ASSERT_TRUE(BZM_hashrate_counter_snapshot(&test_state, counters, BZM_MAX_ASIC_COUNT));
    TEST_ASSERT_EQUAL_UINT32(32, counters[0]);
    TEST_ASSERT_EQUAL_UINT32(0, counters[1]);
    // A duplicate valid result must not earn a second hashrate credit.
    TEST_ASSERT_NULL(BZM_process_work(&test_state));
    TEST_ASSERT_TRUE(BZM_hashrate_counter_snapshot(&test_state, counters, BZM_MAX_ASIC_COUNT));
    TEST_ASSERT_EQUAL_UINT32(32, counters[0]);
    fixture_destroy();
}
