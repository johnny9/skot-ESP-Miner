#include "asic_simulator.h"

#include <pthread.h>
#include <string.h>

typedef struct {
    asic_simulator_snapshot_t snapshot;
    task_result results[ASIC_SIMULATOR_RESULT_CAPACITY];
    task_result current_result;
    size_t result_head;
    size_t result_tail;
} simulator_state_t;

static pthread_mutex_t simulator_lock = PTHREAD_MUTEX_INITIALIZER;
static simulator_state_t simulator = {
    .snapshot = {
        .baud_rate = 115200,
        .job_frequency_ms = 500.0,
    },
};

static uint8_t simulator_init(GlobalState *state)
{
    (void)state;
    pthread_mutex_lock(&simulator_lock);
    ++simulator.snapshot.init_calls;
    uint8_t chip_count = simulator.snapshot.chip_count;
    pthread_mutex_unlock(&simulator_lock);
    return chip_count;
}

static task_result *simulator_process_work(GlobalState *state)
{
    (void)state;
    pthread_mutex_lock(&simulator_lock);
    if (simulator.snapshot.queued_results == 0) {
        pthread_mutex_unlock(&simulator_lock);
        return NULL;
    }

    simulator.current_result = simulator.results[simulator.result_head];
    simulator.result_head = (simulator.result_head + 1) % ASIC_SIMULATOR_RESULT_CAPACITY;
    --simulator.snapshot.queued_results;
    pthread_mutex_unlock(&simulator_lock);
    return &simulator.current_result;
}

static int simulator_set_max_baud(GlobalState *state)
{
    (void)state;
    pthread_mutex_lock(&simulator_lock);
    int baud_rate = simulator.snapshot.baud_rate;
    pthread_mutex_unlock(&simulator_lock);
    return baud_rate;
}

static void simulator_send_work(GlobalState *state, bm_job *job)
{
    (void)state;
    pthread_mutex_lock(&simulator_lock);
    simulator.snapshot.last_job = job;
    ++simulator.snapshot.work_calls;
    pthread_mutex_unlock(&simulator_lock);
}

static void simulator_set_version_mask(GlobalState *state, uint32_t mask)
{
    (void)state;
    pthread_mutex_lock(&simulator_lock);
    simulator.snapshot.version_mask = mask;
    pthread_mutex_unlock(&simulator_lock);
}

static void simulator_set_frequency(GlobalState *state)
{
    (void)state;
    pthread_mutex_lock(&simulator_lock);
    ++simulator.snapshot.frequency_calls;
    pthread_mutex_unlock(&simulator_lock);
}

static void simulator_set_nonce_space(GlobalState *state)
{
    (void)state;
    pthread_mutex_lock(&simulator_lock);
    ++simulator.snapshot.nonce_space_calls;
    pthread_mutex_unlock(&simulator_lock);
}

static double simulator_get_job_frequency_ms(GlobalState *state)
{
    (void)state;
    pthread_mutex_lock(&simulator_lock);
    double job_frequency_ms = simulator.snapshot.job_frequency_ms;
    pthread_mutex_unlock(&simulator_lock);
    return job_frequency_ms;
}

static void simulator_read_registers(GlobalState *state)
{
    (void)state;
    pthread_mutex_lock(&simulator_lock);
    ++simulator.snapshot.register_calls;
    pthread_mutex_unlock(&simulator_lock);
}

static const asic_backend_t backend = {
    .init = simulator_init,
    .process_work = simulator_process_work,
    .set_max_baud = simulator_set_max_baud,
    .send_work = simulator_send_work,
    .set_version_mask = simulator_set_version_mask,
    .set_frequency = simulator_set_frequency,
    .set_nonce_space = simulator_set_nonce_space,
    .get_job_frequency_ms = simulator_get_job_frequency_ms,
    .read_registers = simulator_read_registers,
};

const asic_backend_t *asic_simulator_backend(void)
{
    return &backend;
}

void asic_simulator_reset(void)
{
    pthread_mutex_lock(&simulator_lock);
    memset(&simulator, 0, sizeof(simulator));
    simulator.snapshot.baud_rate = 115200;
    simulator.snapshot.job_frequency_ms = 500.0;
    pthread_mutex_unlock(&simulator_lock);
}

void asic_simulator_configure(uint8_t chip_count, int baud_rate, double job_frequency_ms)
{
    pthread_mutex_lock(&simulator_lock);
    simulator.snapshot.chip_count = chip_count;
    simulator.snapshot.baud_rate = baud_rate;
    simulator.snapshot.job_frequency_ms = job_frequency_ms;
    pthread_mutex_unlock(&simulator_lock);
}

bool asic_simulator_queue_result(const task_result *result)
{
    if (result == NULL) return false;

    pthread_mutex_lock(&simulator_lock);
    if (simulator.snapshot.queued_results == ASIC_SIMULATOR_RESULT_CAPACITY) {
        pthread_mutex_unlock(&simulator_lock);
        return false;
    }

    simulator.results[simulator.result_tail] = *result;
    simulator.result_tail = (simulator.result_tail + 1) % ASIC_SIMULATOR_RESULT_CAPACITY;
    ++simulator.snapshot.queued_results;
    pthread_mutex_unlock(&simulator_lock);
    return true;
}

void asic_simulator_get_snapshot(asic_simulator_snapshot_t *snapshot)
{
    if (snapshot == NULL) return;

    pthread_mutex_lock(&simulator_lock);
    *snapshot = simulator.snapshot;
    pthread_mutex_unlock(&simulator_lock);
}
