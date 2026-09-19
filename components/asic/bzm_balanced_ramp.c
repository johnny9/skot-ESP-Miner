#include "bzm_balanced_ramp.h"

#include <math.h>
#include <string.h>

#include "bzm_registers.h"

#define BZM_BALANCED_RAMP_RESET_DELAY_MS 1U
#define BZM_BALANCED_RAMP_BUSY_POLL_ATTEMPTS 3U
#define BZM_BALANCED_RAMP_BUSY_POLL_DELAY_MS 1U

typedef struct
{
    const bzm_balanced_ramp_ops_t * ops;
    void * ops_context;
    uint8_t asic_id;
} addressed_writer_t;

static bool addressed_write(void * context, uint16_t engine_id, uint8_t offset, const void * data, size_t data_len)
{
    addressed_writer_t * writer = context;
    return writer != NULL && writer->ops->write_register(writer->ops_context, writer->asic_id, engine_id, offset, data,
                                                          data_len);
}

static bool ops_are_complete(const bzm_balanced_ramp_ops_t * ops)
{
    return ops != NULL && ops->begin_engine != NULL && ops->write_register != NULL && ops->read_register != NULL &&
           ops->delay_ms != NULL && ops->telemetry_sample != NULL &&
           ops->final_barrier != NULL;
}

static int asic_index(uint8_t asic_id)
{
    size_t index = 0;
    if (!bzm_topology_asic_index(asic_id, &index)) {
        return -1;
    }
    return (int) index;
}

static bool fail(bzm_balanced_ramp_t * ramp, bzm_balanced_ramp_failure_t failure, uint8_t asic_id, uint16_t engine_id,
                 uint8_t register_offset, uint32_t expected, uint32_t actual)
{
    if (ramp != NULL) {
        ramp->failed = true;
        ramp->failure = failure;
        ramp->failure_asic_id = asic_id;
        ramp->failure_engine_id = engine_id;
        ramp->failure_register_offset = register_offset;
        ramp->failure_expected = expected;
        ramp->failure_actual = actual;
    }
    return false;
}

static bool activate_engine(bzm_balanced_ramp_t * ramp, const bzm_balanced_ramp_ops_t * ops, void * ops_context,
                            uint8_t asic_id, const bzm_engine_location_t * engine)
{
    if (!ops->begin_engine(ops_context, asic_id, engine->physical_id)) {
        return fail(ramp, BZM_BALANCED_RAMP_FAILURE_LEASE, asic_id, engine->physical_id, 0, 1, 0);
    }

    addressed_writer_t writer = {
        .ops = ops,
        .ops_context = ops_context,
        .asic_id = asic_id,
    };
    uint8_t config = BZM_BALANCED_RAMP_ENGINE_CONFIG;
    if (!ops->write_register(ops_context, asic_id, engine->physical_id, BZM_ENGINE_REG_CONFIG, &config, sizeof(config))) {
        return fail(ramp, BZM_BALANCED_RAMP_FAILURE_CONFIG_WRITE, asic_id, engine->physical_id, BZM_ENGINE_REG_CONFIG,
                    config, 0);
    }
    if (!bzm_transport_program_startup_work(engine->physical_id, addressed_write, &writer)) {
        return fail(ramp, BZM_BALANCED_RAMP_FAILURE_SENTINEL_WRITE, asic_id, engine->physical_id, 0, 1, 0);
    }

    /* A dword-aligned status read is also an end-to-end UART ordering
     * barrier for all preceding writes to this engine. */
    uint8_t status_and_config[4] = {0};
    bool busy = false;
    for (uint32_t attempt = 0; attempt < BZM_BALANCED_RAMP_BUSY_POLL_ATTEMPTS; ++attempt) {
        if (attempt != 0) {
            ops->delay_ms(ops_context, BZM_BALANCED_RAMP_BUSY_POLL_DELAY_MS);
        }
        if (!ops->read_register(ops_context, asic_id, engine->physical_id, BZM_ENGINE_REG_STATUS, status_and_config,
                                sizeof(status_and_config))) {
            if (attempt + 1U == BZM_BALANCED_RAMP_BUSY_POLL_ATTEMPTS) {
                return fail(ramp, BZM_BALANCED_RAMP_FAILURE_STATUS_READ, asic_id, engine->physical_id,
                            BZM_ENGINE_REG_STATUS, 1, 0);
            }
            continue;
        }
        if ((status_and_config[0] & BZM_BALANCED_RAMP_ENGINE_BUSY_MASK) != 0) {
            busy = true;
            break;
        }
    }
    if (!busy) {
        return fail(ramp, BZM_BALANCED_RAMP_FAILURE_NOT_BUSY, asic_id, engine->physical_id, BZM_ENGINE_REG_STATUS,
                    BZM_BALANCED_RAMP_ENGINE_BUSY_MASK, status_and_config[0]);
    }
    uint8_t config_readback = status_and_config[1];
    if ((config_readback & BZM_BALANCED_RAMP_ENGINE_CONFIG_WRITABLE_MASK) != BZM_BALANCED_RAMP_ENGINE_CONFIG ||
        (config_readback & (uint8_t) ~BZM_BALANCED_RAMP_ENGINE_CONFIG_ALLOWED_MASK) != 0) {
        return fail(ramp, BZM_BALANCED_RAMP_FAILURE_CONFIG_READBACK, asic_id, engine->physical_id, BZM_ENGINE_REG_CONFIG,
                    BZM_BALANCED_RAMP_ENGINE_CONFIG, config_readback);
    }

    ++ramp->completed_engines;
    return true;
}

static bool prepare_asic(bzm_balanced_ramp_t * ramp, const bzm_balanced_ramp_ops_t * ops, void * ops_context,
                         uint8_t asic_id, int index)
{
    if (ramp->prepared_asic[index]) {
        return true;
    }
    if (!ops->begin_engine(ops_context, asic_id, BZM_CONTROL_ENGINE_ID)) {
        return fail(ramp, BZM_BALANCED_RAMP_FAILURE_LEASE, asic_id, BZM_CONTROL_ENGINE_ID,
                    BZM_LOCAL_REG_ENGINE_SOFT_RESET, 1, 0);
    }

    uint32_t reset = 0;
    if (!ops->write_register(ops_context, asic_id, BZM_CONTROL_ENGINE_ID, BZM_LOCAL_REG_ENGINE_SOFT_RESET, &reset,
                             sizeof(reset))) {
        return fail(ramp, BZM_BALANCED_RAMP_FAILURE_ENGINE_RESET, asic_id, BZM_CONTROL_ENGINE_ID,
                    BZM_LOCAL_REG_ENGINE_SOFT_RESET, 0, 1);
    }
    ops->delay_ms(ops_context, BZM_BALANCED_RAMP_RESET_DELAY_MS);
    reset = 1;
    if (!ops->write_register(ops_context, asic_id, BZM_CONTROL_ENGINE_ID, BZM_LOCAL_REG_ENGINE_SOFT_RESET, &reset,
                             sizeof(reset))) {
        return fail(ramp, BZM_BALANCED_RAMP_FAILURE_ENGINE_RESET, asic_id, BZM_CONTROL_ENGINE_ID,
                    BZM_LOCAL_REG_ENGINE_SOFT_RESET, 1, 0);
    }
    ops->delay_ms(ops_context, BZM_BALANCED_RAMP_RESET_DELAY_MS);
    ramp->prepared_asic[index] = true;
    return true;
}

void bzm_balanced_ramp_init(bzm_balanced_ramp_t * ramp)
{
    if (ramp != NULL) {
        memset(ramp, 0, sizeof(*ramp));
    }
}

const char * bzm_balanced_ramp_failure_name(bzm_balanced_ramp_failure_t failure)
{
    switch (failure) {
    case BZM_BALANCED_RAMP_FAILURE_NONE:
        return "none";
    case BZM_BALANCED_RAMP_FAILURE_ARGUMENT:
        return "argument";
    case BZM_BALANCED_RAMP_FAILURE_TELEMETRY:
        return "telemetry";
    case BZM_BALANCED_RAMP_FAILURE_LEASE:
        return "lease";
    case BZM_BALANCED_RAMP_FAILURE_ENGINE_RESET:
        return "engine_reset";
    case BZM_BALANCED_RAMP_FAILURE_CONFIG_WRITE:
        return "config_write";
    case BZM_BALANCED_RAMP_FAILURE_SENTINEL_WRITE:
        return "sentinel_write";
    case BZM_BALANCED_RAMP_FAILURE_STATUS_READ:
        return "status_read";
    case BZM_BALANCED_RAMP_FAILURE_NOT_BUSY:
        return "not_busy";
    case BZM_BALANCED_RAMP_FAILURE_CONFIG_READBACK:
        return "config_readback";
    case BZM_BALANCED_RAMP_FAILURE_INCOMPLETE:
        return "incomplete";
    case BZM_BALANCED_RAMP_FAILURE_FINAL_BARRIER:
        return "final_barrier";
    default:
        return "invalid";
    }
}

bool bzm_balanced_ramp_commit_pair(bzm_balanced_ramp_t * ramp, const bzm_balanced_ramp_ops_t * ops, void * ops_context,
                                   uint8_t asic_id, const bzm_engine_pair_t * pair)
{
    int index = asic_index(asic_id);
    if (ramp != NULL && ramp->failed) {
        return false;
    }
    if (ramp == NULL || !ops_are_complete(ops) || pair == NULL || index < 0 ||
        pair->pair_index >= BZM_TOPOLOGY_PAIR_COUNT || pair->bottom.stack != BZM_ENGINE_STACK_BOTTOM ||
        pair->top.stack != BZM_ENGINE_STACK_TOP || pair->bottom.stack_index != pair->pair_index ||
        pair->top.stack_index != pair->pair_index || ramp->next_pair[index] != pair->pair_index) {
        return fail(ramp, BZM_BALANCED_RAMP_FAILURE_ARGUMENT, asic_id, pair != NULL ? pair->bottom.physical_id : 0, 0, 1,
                    0);
    }

    if (!prepare_asic(ramp, ops, ops_context, asic_id, index)) {
        return false;
    }

    bzm_telemetry_sample_t sample = {0};
    if (!ops->telemetry_sample(ops_context, asic_id, &sample) || !sample.received || !sample.valid ||
        !isfinite(sample.ch0_mv) || !isfinite(sample.ch1_mv)) {
        return fail(ramp, BZM_BALANCED_RAMP_FAILURE_TELEMETRY, asic_id, 0, 0, 1, sample.valid);
    }

    /* Reference BIRDS firmware starts on the higher stack. Equal readings
     * deterministically select bottom first. */
    const bzm_engine_location_t * first = sample.ch0_mv >= sample.ch1_mv ? &pair->bottom : &pair->top;
    const bzm_engine_location_t * second = first == &pair->bottom ? &pair->top : &pair->bottom;
    if (!activate_engine(ramp, ops, ops_context, asic_id, first) ||
        !activate_engine(ramp, ops, ops_context, asic_id, second)) {
        return false;
    }

    ++ramp->next_pair[index];
    ++ramp->completed_pairs;
    return true;
}

bool bzm_balanced_ramp_barrier(bzm_balanced_ramp_t * ramp, const bzm_balanced_ramp_ops_t * ops, void * ops_context,
                               size_t asic_count, size_t pairs_per_asic)
{
    if (ramp == NULL || !ops_are_complete(ops) || ramp->failed ||
        asic_count != BZM_BRINGUP_ASIC_COUNT || pairs_per_asic != BZM_TOPOLOGY_PAIR_COUNT ||
        ramp->completed_pairs != BZM_BRINGUP_ASIC_COUNT * BZM_TOPOLOGY_PAIR_COUNT ||
        ramp->completed_engines != BZM_BRINGUP_ASIC_COUNT * BZM_TOPOLOGY_ENGINE_COUNT) {
        return fail(ramp, BZM_BALANCED_RAMP_FAILURE_INCOMPLETE, 0, 0, 0,
                    BZM_BRINGUP_ASIC_COUNT * BZM_TOPOLOGY_PAIR_COUNT,
                    ramp != NULL ? ramp->completed_pairs : 0);
    }
    for (size_t index = 0; index < BZM_BRINGUP_ASIC_COUNT; ++index) {
        if (ramp->next_pair[index] != BZM_TOPOLOGY_PAIR_COUNT) {
            return fail(ramp, BZM_BALANCED_RAMP_FAILURE_INCOMPLETE,
                        bzm_asic_wire_ids[index], 0, 0, BZM_TOPOLOGY_PAIR_COUNT,
                        ramp->next_pair[index]);
        }
    }

    if (!ops->final_barrier(ops_context)) {
        return fail(ramp, BZM_BALANCED_RAMP_FAILURE_FINAL_BARRIER, 0, 0, 0, 1, 0);
    }
    return true;
}
