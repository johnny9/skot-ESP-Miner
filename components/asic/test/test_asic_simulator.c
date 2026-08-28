#include "unity.h"

#include "asic.h"
#include "asic_backend.h"
#include "asic_simulator.h"

#include <stdint.h>

TEST_CASE("ASIC simulator records work and returns results", "[asic_simulator]")
{
    GlobalState *state = (GlobalState *)(uintptr_t)0x1234;
    bm_job *job = (bm_job *)(uintptr_t)0x4321;
    task_result expected = {
        .job_id = 7,
        .nonce = 0x89abcdef,
        .rolled_version = 0x20002000,
        .timestamp_us = 123456,
    };

    asic_simulator_reset();
    asic_simulator_configure(2, 3000000, 25.5);
    ASIC_set_backend(asic_simulator_backend());

    TEST_ASSERT_EQUAL_UINT8(2, ASIC_init(state));
    TEST_ASSERT_EQUAL(3000000, ASIC_set_max_baud(state));
    TEST_ASSERT_EQUAL_DOUBLE(25.5, ASIC_get_asic_job_frequency_ms(state));
    ASIC_send_work(state, job);
    ASIC_set_version_mask(state, 0x1fffe000);
    ASIC_set_frequency(state);
    ASIC_set_nonce_space(state);
    ASIC_read_registers(state);
    TEST_ASSERT_TRUE(asic_simulator_queue_result(&expected));

    task_result *actual = ASIC_process_work(state);
    TEST_ASSERT_NOT_NULL(actual);
    TEST_ASSERT_EQUAL_UINT8(expected.job_id, actual->job_id);
    TEST_ASSERT_EQUAL_HEX32(expected.nonce, actual->nonce);
    TEST_ASSERT_EQUAL_HEX32(expected.rolled_version, actual->rolled_version);
    TEST_ASSERT_EQUAL_UINT64(expected.timestamp_us, actual->timestamp_us);
    TEST_ASSERT_NULL(ASIC_process_work(state));

    asic_simulator_snapshot_t snapshot;
    asic_simulator_get_snapshot(&snapshot);
    TEST_ASSERT_EQUAL_PTR(job, snapshot.last_job);
    TEST_ASSERT_EQUAL_HEX32(0x1fffe000, snapshot.version_mask);
    TEST_ASSERT_EQUAL_UINT32(1, snapshot.init_calls);
    TEST_ASSERT_EQUAL_UINT32(1, snapshot.work_calls);
    TEST_ASSERT_EQUAL_UINT32(1, snapshot.frequency_calls);
    TEST_ASSERT_EQUAL_UINT32(1, snapshot.nonce_space_calls);
    TEST_ASSERT_EQUAL_UINT32(1, snapshot.register_calls);
    TEST_ASSERT_EQUAL_UINT32(0, snapshot.queued_results);

    ASIC_reset_backend();
}

TEST_CASE("ASIC simulator rejects a full result queue", "[asic_simulator]")
{
    task_result result = {0};
    asic_simulator_reset();
    ASIC_set_backend(asic_simulator_backend());

    TEST_ASSERT_EQUAL_UINT8(0, ASIC_init(NULL));
    TEST_ASSERT_FALSE(asic_simulator_queue_result(NULL));
    for (size_t i = 0; i < ASIC_SIMULATOR_RESULT_CAPACITY; i++) {
        result.job_id = (uint8_t)i;
        TEST_ASSERT_TRUE(asic_simulator_queue_result(&result));
    }
    TEST_ASSERT_FALSE(asic_simulator_queue_result(&result));

    for (size_t i = 0; i < ASIC_SIMULATOR_RESULT_CAPACITY; i++) {
        task_result *actual = ASIC_process_work(NULL);
        TEST_ASSERT_NOT_NULL(actual);
        TEST_ASSERT_EQUAL_UINT8(i, actual->job_id);
    }
    TEST_ASSERT_NULL(ASIC_process_work(NULL));
    ASIC_reset_backend();
}
