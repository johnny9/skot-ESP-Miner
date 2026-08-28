#include "unity.h"

#include "asic.h"
#include "asic_backend.h"

#include <stdint.h>

static GlobalState *last_state;
static bm_job *last_job;
static uint32_t last_mask;
static unsigned call_count;

static uint8_t fake_init(GlobalState *state)
{
    last_state = state;
    ++call_count;
    return 3;
}

static task_result *fake_process_work(GlobalState *state)
{
    last_state = state;
    ++call_count;
    return (task_result *)(uintptr_t)0x5678;
}

static int fake_set_max_baud(GlobalState *state)
{
    last_state = state;
    ++call_count;
    return 115200;
}

static void fake_send_work(GlobalState *state, bm_job *job)
{
    last_state = state;
    last_job = job;
    ++call_count;
}

static void fake_set_version_mask(GlobalState *state, uint32_t mask)
{
    last_state = state;
    last_mask = mask;
    ++call_count;
}

static void fake_call(GlobalState *state)
{
    last_state = state;
    ++call_count;
}

static double fake_get_job_frequency_ms(GlobalState *state)
{
    last_state = state;
    ++call_count;
    return 12.5;
}

static const asic_backend_t fake_backend = {
    .init = fake_init,
    .process_work = fake_process_work,
    .set_max_baud = fake_set_max_baud,
    .send_work = fake_send_work,
    .set_version_mask = fake_set_version_mask,
    .set_frequency = fake_call,
    .set_nonce_space = fake_call,
    .get_job_frequency_ms = fake_get_job_frequency_ms,
    .read_registers = fake_call,
};

TEST_CASE("ASIC facade forwards to selected backend", "[asic_backend]")
{
    GlobalState *state = (GlobalState *)(uintptr_t)0x1234;
    bm_job *job = (bm_job *)(uintptr_t)0x4321;
    call_count = 0;

    ASIC_set_backend(&fake_backend);

    TEST_ASSERT_EQUAL_UINT8(3, ASIC_init(state));
    TEST_ASSERT_EQUAL_PTR((task_result *)(uintptr_t)0x5678, ASIC_process_work(state));
    TEST_ASSERT_EQUAL(115200, ASIC_set_max_baud(state));
    ASIC_send_work(state, job);
    ASIC_set_version_mask(state, 0x1fffe000);
    ASIC_set_frequency(state);
    ASIC_set_nonce_space(state);
    TEST_ASSERT_EQUAL_DOUBLE(12.5, ASIC_get_asic_job_frequency_ms(state));
    ASIC_read_registers(state);

    TEST_ASSERT_EQUAL_PTR(state, last_state);
    TEST_ASSERT_EQUAL_PTR(job, last_job);
    TEST_ASSERT_EQUAL_HEX32(0x1fffe000, last_mask);
    TEST_ASSERT_EQUAL_UINT32(9, call_count);

    ASIC_reset_backend();
}
