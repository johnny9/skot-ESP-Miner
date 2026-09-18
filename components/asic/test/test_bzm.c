#include <stdlib.h>
#include <string.h>

#include "bzm_job_store.h"
#include "bzm.h"
#include "bitmain_job_packet.h"
#include "bzm_bridge.h"
#include "bzm_reactor.h"
#include "bzm_running_evidence.h"
#include "bzm_transport.h"
#include "mining.h"
#include "unity.h"
#include "utils.h"

#define BZM_TEST_MAX_WRITES (BZM_JOB_STORE_CAPACITY + 4U)

typedef struct {
    size_t write_count;
    size_t checkpoint_count;
    size_t flush_count;
    size_t fail_after;
    bool fail_immediately;
    bool fail_checkpoint;
    bool fail_flush;
    bzm_work_t work[BZM_TEST_MAX_WRITES];
} simulated_transport_t;

typedef struct {
    uint16_t engine_id;
    uint8_t offset;
    size_t data_len;
    uint8_t data[32];
} register_write_t;

typedef struct {
    size_t count;
    register_write_t writes[64];
} register_capture_t;

typedef struct {
    size_t available;
    size_t programmed;
    size_t read_count;
    size_t broadcast_noop_count;
    size_t addressed_noop_count;
    uint8_t programmed_ids[BZM_MAX_ASIC_COUNT];
    uint8_t chain_enabled[BZM_MAX_ASIC_COUNT];
} simulated_chain_t;

static bool simulated_noop(void *context, uint8_t asic_id)
{
    simulated_chain_t *chain = context;
    if (asic_id == BZM_BROADCAST_ASIC) {
        chain->broadcast_noop_count++;
        return chain->programmed < chain->available;
    }
    chain->addressed_noop_count++;
    size_t index = 0;
    return bzm_topology_asic_index(asic_id, &index) && index < chain->programmed;
}

static bool simulated_chain_write(void *context, uint8_t asic_id,
                                  uint16_t engine_id, uint8_t offset,
                                  const void *data, size_t data_len)
{
    simulated_chain_t *chain = context;
    const uint8_t *bytes = data;
    if (asic_id != BZM_BROADCAST_ASIC ||
        engine_id != BZM_CONTROL_ENGINE_ID || offset != 0x0b ||
        data_len != 4 || chain->programmed >= chain->available) {
        return false;
    }
    chain->programmed_ids[chain->programmed] = bytes[0];
    chain->chain_enabled[chain->programmed] = bytes[1];
    chain->programmed++;
    return true;
}

static bool simulated_chain_read(void *context, uint8_t asic_id,
                                 uint16_t engine_id, uint8_t offset,
                                 void *data, size_t data_len)
{
    simulated_chain_t *chain = context;
    uint8_t *bytes = data;
    size_t index = 0;
    if (engine_id != BZM_CONTROL_ENGINE_ID || offset != 0x0b ||
        data_len != 4 || !bzm_topology_asic_index(asic_id, &index) || index >= chain->programmed) {
        return false;
    }
    memset(bytes, 0, data_len);
    bytes[0] = chain->programmed_ids[index];
    bytes[1] = chain->chain_enabled[index];
    chain->read_count++;
    return true;
}

static void simulated_chain_delay(void *context, uint32_t delay_ms)
{
    (void)context;
    TEST_ASSERT_EQUAL_UINT32(200, delay_ms);
}

static const bzm_chain_ops_t SIMULATED_CHAIN_OPS = {
    .noop = simulated_noop,
    .write_register = simulated_chain_write,
    .read_register = simulated_chain_read,
    .delay_ms = simulated_chain_delay,
};

static bool capture_register(void *context, uint16_t engine_id,
                             uint8_t offset, const void *data,
                             size_t data_len)
{
    register_capture_t *capture = context;
    if (capture->count >= 64 || data_len > 32) return false;
    register_write_t *write = &capture->writes[capture->count++];
    write->engine_id = engine_id;
    write->offset = offset;
    write->data_len = data_len;
    memcpy(write->data, data, data_len);
    return true;
}

static bool simulated_write(void *context, const bzm_work_t *work)
{
    simulated_transport_t *transport = context;
    if (transport->fail_immediately ||
        transport->write_count >= BZM_TEST_MAX_WRITES ||
        (transport->fail_after != 0 &&
         transport->write_count >= transport->fail_after)) {
        return false;
    }
    transport->work[transport->write_count++] = *work;
    return true;
}

static bool simulated_flush(void *context)
{
    simulated_transport_t *transport = context;
    transport->flush_count++;
    return !transport->fail_flush;
}

static bool simulated_checkpoint(void *context)
{
    simulated_transport_t *transport = context;
    transport->checkpoint_count++;
    return !transport->fail_checkpoint;
}

static const bzm_transport_ops_t SIMULATED_OPS = {
    .write_work = simulated_write,
    .dispatch_checkpoint = simulated_checkpoint,
    .flush = simulated_flush,
};

static simulated_transport_t *new_transport(void)
{
    simulated_transport_t *transport = calloc(1, sizeof(*transport));
    TEST_ASSERT_NOT_NULL(transport);
    return transport;
}

static bzm_job_store_t *new_store(void)
{
    bzm_job_store_t *store = calloc(1, sizeof(*store));
    TEST_ASSERT_NOT_NULL(store);
    TEST_ASSERT_TRUE(bzm_job_store_init(store));
    return store;
}

static void delete_store(bzm_job_store_t *store)
{
    bzm_job_store_destroy(store);
    free(store);
}

static asic_job_t bzm_template(const char *job_id)
{
    asic_job_t template = {
        .version = 0x20000004,
        .version_mask = 0x00006000,
        .ntime = 0x65010203,
        .nbits = 0x1705dd01,
        .starting_nonce = 0x10203040,
        .source_type = JOB_TYPE_SV2_STANDARD,
        .pool_diff = 1,
    };
    for (size_t i = 0; i < 32; ++i) {
        template.prev_hash[i] = 28 - (i / 4) * 4 + i % 4;
        template.merkle_root[i] = 0x20 + 28 - (i / 4) * 4 + i % 4;
    }
    strcpy(template.job_id, job_id);
    strcpy(template.extranonce2, "");
    return template;
}

static bzm_reactor_t *new_reactor(bzm_job_store_t *store,
                                  simulated_transport_t *transport,
                                  uint16_t engine_count)
{
    bzm_reactor_t *reactor = calloc(1, sizeof(*reactor));
    TEST_ASSERT_NOT_NULL(reactor);
    bzm_reactor_config_t config = {
        .engine_count = engine_count,
        .timestamp_count = 16,
        .lead_zeros = 36,
        .nonce_offset = BZM_NONCE_GAP_1002,
        .enhanced_mode = true,
    };
    TEST_ASSERT_TRUE(bzm_reactor_init(reactor, store, &config,
                                      &SIMULATED_OPS, transport));
    return reactor;
}

TEST_CASE("BZM work builder derives four family-private midstates",
          "[asic][bzm][work][qemu-integration]")
{
    asic_job_t template = bzm_template("work");
    template.version_mask = 0x1fffe000;
    bzm_work_ref_t source = {
        .handle = 0x1234,
        .template = &template,
    };
    bzm_work_t work;
    TEST_ASSERT_TRUE(bzm_work_build(
        &source, 7, 5, 16, CONFIG_BZM_1002_LEAD_ZEROS, true, &work));
    TEST_ASSERT_EQUAL_UINT16(7, work.engine_id);
    TEST_ASSERT_EQUAL_UINT8(5, work.logical_sequence);
    TEST_ASSERT_EQUAL_UINT8(4, work.midstate_count);
    TEST_ASSERT_EQUAL_HEX32(0x20000004, work.versions[0]);
    TEST_ASSERT_EQUAL_HEX32(0x20002004, work.versions[1]);
    TEST_ASSERT_EQUAL_HEX32(0x20004004, work.versions[2]);
    TEST_ASSERT_EQUAL_HEX32(0x20006004, work.versions[3]);
    TEST_ASSERT_EQUAL_HEX32(template.ntime, work.start_ntime);
    TEST_ASSERT_EQUAL_HEX32(template.nbits, work.target);
    TEST_ASSERT_EQUAL_HEX32(template.starting_nonce, work.starting_nonce);
    TEST_ASSERT_EQUAL_HEX32(UINT32_MAX, work.end_nonce);

    bm1397_job_packet_t bm;
    bm1397_build_job_packet(&template, 4, 4, &bm);
    for (size_t word = 0; word < 8; ++word) {
        const uint8_t *big_endian_word = bm.midstates[0] + (7 - word) * 4;
        const uint8_t expected_birds_word[4] = {
            big_endian_word[3], big_endian_word[2],
            big_endian_word[1], big_endian_word[0],
        };
        TEST_ASSERT_EQUAL_UINT8_ARRAY(expected_birds_word,
                                      work.midstates[0] + word * 4, 4);
    }

}

TEST_CASE("BZM keeps enhanced sequence identity when version rolling is unavailable",
          "[asic][bzm][reactor][version][sequence][qemu-integration]")
{
    bzm_job_store_t *store = new_store();
    simulated_transport_t *transport = new_transport();
    bzm_reactor_t *reactor = new_reactor(store, transport, 1);
    asic_job_t template = bzm_template("no-version-mask");
    template.version_mask = 0;
    for (unsigned assignment = 0; assignment < 2; ++assignment) {
        bzm_work_t work;
        TEST_ASSERT_EQUAL(BZM_ASSIGN_OK, bzm_reactor_assign(reactor, &template, &work));
        TEST_ASSERT_EQUAL_UINT8(4, work.midstate_count);
        for (unsigned variant = 0; variant < 4; ++variant) {
            TEST_ASSERT_EQUAL_HEX32(template.version, work.versions[variant]);
            bzm_raw_result_t raw = {
                .asic_id = BZM_FIRST_ASIC_ID, .engine_id = work.engine_id,
                .sequence_id = (work.logical_sequence << 2) | variant,
                .status = 8, .time = 16,
            };
            bzm_result_t event;
            TEST_ASSERT_TRUE(bzm_reactor_map_result(reactor, &raw, &event));
            TEST_ASSERT_EQUAL_HEX32(0, event.version_bits);
        }
    }

    free(transport);
    free(reactor);
    delete_store(store);
}

TEST_CASE("BZM result frame decoder follows the mixed-endian wire layout",
          "[asic][bzm][result][qemu-integration]")
{
    uint8_t frame[BZM_RESULT_FRAME_SIZE] = {
        0x83, 0x45, 0x78, 0x56, 0x34, 0x12, 0x17, 0x0d,
    };
    bzm_raw_result_t result;
    TEST_ASSERT_TRUE(bzm_result_decode(frame, 999, &result));
    TEST_ASSERT_EQUAL_UINT16(0x345, result.engine_id);
    TEST_ASSERT_EQUAL_UINT8(8, result.status);
    TEST_ASSERT_EQUAL_HEX32(0x12345678, result.nonce);
    TEST_ASSERT_EQUAL_UINT8(0x17, result.sequence_id);
    TEST_ASSERT_EQUAL_UINT8(0x0d, result.time);
    TEST_ASSERT_EQUAL_UINT32(999, (uint32_t)result.timestamp_us);
}

TEST_CASE("BZM TDM result decoder preserves the ASIC address",
          "[asic][bzm][result][qemu-integration]")
{
    uint8_t frame[BZM_TDM_RESULT_FRAME_SIZE] = {
        0x1e, 0x01,
        0x83, 0x45, 0x78, 0x56, 0x34, 0x12, 0x17, 0x0d,
    };
    bzm_raw_result_t result;
    TEST_ASSERT_TRUE(bzm_tdm_result_decode(frame, 999, &result));
    TEST_ASSERT_EQUAL_HEX8(0x1e, result.asic_id);
    TEST_ASSERT_EQUAL_UINT16(0x345, result.engine_id);
    frame[1] = 0x03;
    TEST_ASSERT_FALSE(bzm_tdm_result_decode(frame, 999, &result));
}

TEST_CASE("BZM temperature conversion follows the Intel 12-bit formula",
          "[asic][bzm][temperature][qemu-integration]")
{
    TEST_ASSERT_FLOAT_WITHIN(0.01f, 22.02f,
                             bzm_temperature_from_code(0x800));
}

TEST_CASE("BZM compact engine IDs skip every disabled 1002 coordinate",
          "[asic][bzm][engine-map][qemu-integration]")
{
    TEST_ASSERT_EQUAL_UINT16(236, BZM_ENGINES_PER_ASIC);
    TEST_ASSERT_EQUAL_UINT16(240, BZM_ENGINE_GRID_COUNT);

    for (uint16_t logical = 0; logical < BZM_ENGINES_PER_ASIC; ++logical) {
        uint16_t physical;
        uint16_t round_trip;
        bzm_engine_location_t expected;
        TEST_ASSERT_TRUE(bzm_topology_engine_at(logical, &expected));
        TEST_ASSERT_TRUE(bzm_engine_physical_id(logical, &physical));
        TEST_ASSERT_EQUAL_UINT16(expected.physical_id, physical);
        TEST_ASSERT_TRUE(bzm_engine_logical_id(physical, &round_trip));
        TEST_ASSERT_EQUAL_UINT16(logical, round_trip);
    }

    static const struct {
        uint16_t logical;
        uint16_t physical;
    } boundaries[] = {
        {0, 0},
        {19, 19},
        {20, 64},
        {79, 211},
        {80, 257},
        {98, 275},
        {99, 321},
        {116, 338},
        {117, 384},
        {235, 722},
    };
    for (size_t i = 0; i < sizeof(boundaries) / sizeof(boundaries[0]); ++i) {
        uint16_t physical;
        TEST_ASSERT_TRUE(bzm_engine_physical_id(boundaries[i].logical,
                                                &physical));
        TEST_ASSERT_EQUAL_UINT16(boundaries[i].physical, physical);
    }

    uint16_t value;
    TEST_ASSERT_FALSE(bzm_engine_physical_id(BZM_ENGINES_PER_ASIC, &value));
    TEST_ASSERT_FALSE(bzm_engine_physical_id(0, NULL));
    TEST_ASSERT_FALSE(bzm_engine_logical_id(0, NULL));
    TEST_ASSERT_FALSE(bzm_engine_logical_id(20, &value));
    TEST_ASSERT_FALSE(bzm_engine_logical_id(256, &value));
    TEST_ASSERT_FALSE(bzm_engine_logical_id(320, &value));
    TEST_ASSERT_FALSE(bzm_engine_logical_id(339, &value));
    TEST_ASSERT_FALSE(bzm_engine_logical_id(723, &value));
}

TEST_CASE("BZM transport encoder emits byte-paired 9-bit write words",
          "[asic][bzm][transport][qemu-integration]")
{
    TEST_ASSERT_EQUAL_HEX8(0xfa, BZM_BROADCAST_ASIC);
    TEST_ASSERT_EQUAL_HEX8(0xff, BZM_ALL_ASICS);
    uint8_t encoded[32];
    uint8_t data[] = {0xaa, 0xbb};
    size_t length = bzm_transport_encode_write(
        0xfa, 0x345, 0x40, data, sizeof(data), encoded,
        sizeof(encoded));
    const uint8_t expected[] = {
        0xfa, 0x01, // address word 0x1fa
        0x23, 0x00, // write opcode plus engine high nibble
        0x45, 0x00, // engine low byte
        0x40, 0x00, // register
        0x01, 0x00, // byte count minus one
        0xaa, 0x00,
        0xbb, 0x00,
        0x00, 0x00,
    };
    TEST_ASSERT_EQUAL(sizeof(expected), length);
    TEST_ASSERT_EQUAL_UINT8_ARRAY(expected, encoded, sizeof(expected));
    TEST_ASSERT_EQUAL(0, bzm_transport_encode_write(
        0, BZM_MAX_ENGINE_COUNT, 0, data, sizeof(data), encoded,
        sizeof(encoded)));
}

TEST_CASE("BZM transport encodes read and noop commands",
          "[asic][bzm][transport][qemu-integration]")
{
    uint8_t encoded[16];
    const uint8_t expected_read[] = {
        0x42, 0x01,
        0x3f, 0x00,
        0xff, 0x00,
        0x0b, 0x00,
        0x03, 0x00,
        0x00, 0x00,
    };
    TEST_ASSERT_EQUAL(sizeof(expected_read), bzm_transport_encode_read(
        0x42, BZM_CONTROL_ENGINE_ID, 0x0b, 4, encoded,
        sizeof(encoded)));
    TEST_ASSERT_EQUAL_UINT8_ARRAY(expected_read, encoded,
                                  sizeof(expected_read));

    const uint8_t expected_noop[] = {
        0xfa, 0x01,
        0xf0, 0x00,
    };
    TEST_ASSERT_EQUAL(sizeof(expected_noop), bzm_transport_encode_noop(
        BZM_BROADCAST_ASIC, encoded, sizeof(encoded)));
    TEST_ASSERT_EQUAL_UINT8_ARRAY(expected_noop, encoded,
                                  sizeof(expected_noop));
}

TEST_CASE("BZM chain discovery reports every chain length from zero to four",
          "[asic][bzm][discovery][qemu-integration]")
{
    for (size_t available = 0; available <= BZM_MAX_ASIC_COUNT;
         ++available) {
        uint8_t ids[BZM_MAX_ASIC_COUNT] = {0};
        simulated_chain_t chain = {.available = available};
        TEST_ASSERT_EQUAL_UINT32(available, bzm_discover_chain(
            BZM_MAX_ASIC_COUNT, ids, sizeof(ids),
            &SIMULATED_CHAIN_OPS, &chain));
        TEST_ASSERT_EQUAL_UINT32(available, chain.read_count);
        TEST_ASSERT_EQUAL_UINT32(0, chain.addressed_noop_count);
        for (size_t i = 0; i < available; ++i) {
            TEST_ASSERT_EQUAL_UINT8(bzm_asic_wire_ids[i], ids[i]);
            TEST_ASSERT_EQUAL_UINT8(bzm_asic_wire_ids[i], chain.programmed_ids[i]);
            TEST_ASSERT_EQUAL_UINT8(i == 0 ? 0 : 1,
                                    chain.chain_enabled[i]);
        }
    }
}

TEST_CASE("BZM transport partitions an engine nonce range across ASICs",
          "[asic][bzm][transport][nonce][qemu-integration]")
{
    for (size_t i = 0; i < 4; ++i) {
        uint32_t start;
        uint32_t end;
        TEST_ASSERT_TRUE(bzm_partition_nonce_range(
            0x10000000, 0x1fffffff, i, 4, &start, &end));
        TEST_ASSERT_EQUAL_HEX32(0x10000000 + i * 0x04000000,
                                start);
        TEST_ASSERT_EQUAL_HEX32(0x13ffffff + i * 0x04000000,
                                end);
    }
    uint32_t start;
    uint32_t end;
    TEST_ASSERT_FALSE(bzm_partition_nonce_range(
        4, 3, 0, 1, &start, &end));
}

TEST_CASE("BZM transport programs ordered enhanced work and flush jobs",
          "[asic][bzm][transport][program][qemu-integration]")
{
    asic_job_t template = bzm_template("transport");
    bzm_work_ref_t source = {
        .handle = 0x1234,
        .template = &template,
    };
    bzm_work_t work;
    TEST_ASSERT_TRUE(bzm_work_build(
        &source, 7, 5, 16, CONFIG_BZM_1002_LEAD_ZEROS, true, &work));

    register_capture_t capture = {0};
    TEST_ASSERT_TRUE(bzm_transport_program_work(
        &work, capture_register, &capture));
    TEST_ASSERT_EQUAL_UINT32(16, capture.count);
    TEST_ASSERT_EQUAL_HEX8(0x49, capture.writes[0].offset);
    TEST_ASSERT_EQUAL_UINT8(CONFIG_BZM_1002_LEAD_ZEROS - 32,
                            capture.writes[0].data[0]);
    TEST_ASSERT_EQUAL_HEX8(0x90, capture.writes[1].data[0]);
    static const uint8_t expected_merkle_residue[] = {0x23, 0x22, 0x21, 0x20};
    static const uint8_t expected_ntime[] = {0x65, 0x01, 0x02, 0x03};
    static const uint8_t expected_target[] = {0x17, 0x05, 0xdd, 0x01};
    TEST_ASSERT_EQUAL_UINT8_ARRAY(expected_merkle_residue, capture.writes[4].data, 4);
    TEST_ASSERT_EQUAL_UINT8_ARRAY(expected_ntime, capture.writes[5].data, 4);
    TEST_ASSERT_EQUAL_UINT8_ARRAY(expected_target, capture.writes[6].data, 4);
    for (size_t i = 7; i < 11; ++i) {
        TEST_ASSERT_EQUAL_HEX8(0x10, capture.writes[i].offset);
    }
    for (size_t i = 11; i < 15; ++i) {
        TEST_ASSERT_EQUAL_HEX8(0x38, capture.writes[i].offset);
        TEST_ASSERT_EQUAL_UINT8(20 + (i - 11),
                                capture.writes[i].data[0]);
    }
    TEST_ASSERT_EQUAL_HEX8(0x39, capture.writes[15].offset);
    TEST_ASSERT_EQUAL_UINT8(3, capture.writes[15].data[0]);

    capture = (register_capture_t){0};
    TEST_ASSERT_TRUE(bzm_transport_program_broadcast_work(
        &work, capture_register, &capture));
    TEST_ASSERT_EQUAL_UINT32(14, capture.count);
    for (size_t i = 0; i < capture.count; ++i) {
        TEST_ASSERT_NOT_EQUAL_HEX8(0x3c, capture.writes[i].offset);
        TEST_ASSERT_NOT_EQUAL_HEX8(0x40, capture.writes[i].offset);
    }
    TEST_ASSERT_EQUAL_HEX8(0x30, capture.writes[2].offset);
    TEST_ASSERT_EQUAL_HEX8(0x39, capture.writes[13].offset);

    TEST_ASSERT_TRUE(bzm_result_queue_capacity_covers(
        BZM_PENDING_RESULT_COUNT,
        BZM_RESULT_DESIGN_RATE_PER_SECOND,
        BZM_RESULT_MAX_DISPATCH_BLACKOUT_MS));
    TEST_ASSERT_FALSE(bzm_result_queue_capacity_covers(
        32U,
        BZM_RESULT_DESIGN_RATE_PER_SECOND,
        BZM_RESULT_MAX_DISPATCH_BLACKOUT_MS));

    memset(&capture, 0, sizeof(capture));
    TEST_ASSERT_TRUE(bzm_transport_program_flush(
        1, true, capture_register, &capture));
    TEST_ASSERT_EQUAL_UINT32(12, capture.count);
    TEST_ASSERT_EQUAL_HEX8(0xff, capture.writes[0].data[0]);
    for (size_t i = 1; i < 5; ++i) {
        TEST_ASSERT_EQUAL_UINT8(0xfb + i, capture.writes[i].data[0]);
    }
    TEST_ASSERT_EQUAL_UINT8(3, capture.writes[5].data[0]);
    TEST_ASSERT_EQUAL_UINT8(1, capture.writes[11].data[0]);

    memset(&capture, 0, sizeof(capture));
    TEST_ASSERT_TRUE(bzm_transport_program_stage6_sentinel(
        10, capture_register, &capture));
    TEST_ASSERT_EQUAL_UINT32(22, capture.count);
    TEST_ASSERT_EQUAL_UINT16(10, capture.writes[0].engine_id);
    TEST_ASSERT_EQUAL_HEX8(0x49, capture.writes[0].offset);
    TEST_ASSERT_EQUAL_UINT8(32, capture.writes[0].data[0]);
    TEST_ASSERT_EQUAL_HEX8(0xff, capture.writes[1].data[0]);
    for (size_t i = 11; i < 15; ++i) {
        TEST_ASSERT_EQUAL_HEX8(0x38, capture.writes[i].offset);
        TEST_ASSERT_EQUAL_UINT8(0xfc + (i - 11), capture.writes[i].data[0]);
    }
    TEST_ASSERT_EQUAL_HEX8(0x39, capture.writes[15].offset);
    TEST_ASSERT_EQUAL_UINT8(3, capture.writes[15].data[0]);
    TEST_ASSERT_EQUAL_HEX8(0xff, capture.writes[16].data[0]);
    TEST_ASSERT_EQUAL_HEX8(0x39, capture.writes[21].offset);
    TEST_ASSERT_EQUAL_UINT8(1, capture.writes[21].data[0]);

    memset(&capture, 0, sizeof(capture));
    TEST_ASSERT_TRUE(bzm_transport_program_flush(
        2, true, capture_register, &capture));
    TEST_ASSERT_EQUAL_UINT32(24, capture.count);
    TEST_ASSERT_EQUAL_UINT16(0, capture.writes[0].engine_id);
    TEST_ASSERT_EQUAL_UINT16(10, capture.writes[12].engine_id);

}

TEST_CASE("BZM reactor dispatches one stored generation to every engine",
          "[asic][bzm][reactor][qemu-integration]")
{
    bzm_job_store_t *store = new_store();
    simulated_transport_t *transport = new_transport();
    bzm_reactor_t *reactor = new_reactor(store, transport, 4);
    asic_job_t template = bzm_template("dispatch");

    size_t assigned = 0;
    TEST_ASSERT_EQUAL(BZM_ASSIGN_OK,
                      bzm_reactor_dispatch(reactor, &template, &assigned));
    TEST_ASSERT_EQUAL_UINT32(4, assigned);
    TEST_ASSERT_EQUAL_UINT32(4, transport->write_count);
    TEST_ASSERT_EQUAL_UINT32(4, transport->checkpoint_count);
    bzm_work_handle_t shared = transport->work[0].source.handle;
    int bottom_count = 0;
    int top_count = 0;
    for (size_t i = 0; i < assigned; ++i) {
        bzm_engine_location_t expected;
        TEST_ASSERT_TRUE(bzm_topology_activation_at(
            i, BZM_ENGINE_STACK_BOTTOM, &expected));
        TEST_ASSERT_EQUAL_UINT32(expected.physical_id,
                                 transport->work[i].engine_id);
        if (expected.stack == BZM_ENGINE_STACK_BOTTOM) {
            ++bottom_count;
        } else {
            ++top_count;
        }
        TEST_ASSERT_LESS_OR_EQUAL_INT(1, abs(bottom_count - top_count));
        TEST_ASSERT_EQUAL_HEX32((uint32_t)shared,
                                (uint32_t)transport->work[i].source.handle);
        TEST_ASSERT_EQUAL_HEX32((uint32_t)(shared >> 32),
                                (uint32_t)(transport->work[i].source.handle >> 32));
        TEST_ASSERT_EQUAL_HEX32(i * 0x40000000U,
                                transport->work[i].starting_nonce);
        TEST_ASSERT_EQUAL_HEX32(((i + 1) * 0x40000000ULL) - 1,
                                transport->work[i].end_nonce);
    }

    asic_job_t snapshot;
    TEST_ASSERT_TRUE(bzm_job_store_snapshot(store, shared, &snapshot));
    TEST_ASSERT_EQUAL_STRING("dispatch", snapshot.job_id);

    free(transport);
    free(reactor);
    delete_store(store);
}

TEST_CASE("BZM full dispatch covers 236 engines in balanced write order",
          "[asic][bzm][reactor][topology][balance][qemu-integration]")
{
    bzm_job_store_t *store = new_store();
    simulated_transport_t *transport = new_transport();
    bzm_reactor_t *reactor = new_reactor(
        store, transport, BZM_ENGINES_PER_ASIC);
    asic_job_t template = bzm_template("full-topology");
    bool seen[BZM_ENGINE_GRID_COUNT] = {false};

    size_t assigned = 0;
    TEST_ASSERT_EQUAL(BZM_ASSIGN_OK,
                      bzm_reactor_dispatch(reactor, &template, &assigned));
    TEST_ASSERT_EQUAL_UINT16(BZM_ENGINES_PER_ASIC, assigned);
    TEST_ASSERT_EQUAL_UINT16(BZM_ENGINES_PER_ASIC,
                             transport->write_count);
    TEST_ASSERT_EQUAL_UINT16(BZM_ENGINES_PER_ASIC,
                             transport->checkpoint_count);

    int bottom_count = 0;
    int top_count = 0;
    for (size_t schedule_index = 0; schedule_index < assigned;
         ++schedule_index) {
        bzm_engine_location_t expected;
        TEST_ASSERT_TRUE(bzm_topology_activation_at(
            schedule_index, BZM_ENGINE_STACK_BOTTOM, &expected));
        TEST_ASSERT_EQUAL_UINT16(expected.physical_id,
                                 transport->work[schedule_index].engine_id);
        TEST_ASSERT_FALSE(seen[expected.grid_id]);
        seen[expected.grid_id] = true;

        if (expected.stack == BZM_ENGINE_STACK_BOTTOM) {
            ++bottom_count;
        } else {
            ++top_count;
        }
        TEST_ASSERT_LESS_OR_EQUAL_INT(1, abs(bottom_count - top_count));
    }
    TEST_ASSERT_EQUAL_INT(BZM_TOPOLOGY_STACK_ENGINE_COUNT, bottom_count);
    TEST_ASSERT_EQUAL_INT(BZM_TOPOLOGY_STACK_ENGINE_COUNT, top_count);

    // Balanced ordering limits transient skew, but does not claim an atomic
    // pairwise commit. The separate partial-dispatch test verifies fail/flush.
    TEST_ASSERT_FALSE(seen[4 * BZM_ENGINE_ROWS]);
    TEST_ASSERT_FALSE(seen[5 * BZM_ENGINE_ROWS]);
    TEST_ASSERT_FALSE(seen[5 * BZM_ENGINE_ROWS + 19]);
    TEST_ASSERT_FALSE(seen[11 * BZM_ENGINE_ROWS + 19]);

    free(transport);
    free(reactor);
    delete_store(store);
}

TEST_CASE("BZM incremental assignments retain compact IDs in balanced order",
          "[asic][bzm][reactor][topology][result][qemu-integration]")
{
    bzm_job_store_t *store = new_store();
    simulated_transport_t *transport = new_transport();
    bzm_reactor_t *reactor = calloc(1, sizeof(*reactor));
    TEST_ASSERT_NOT_NULL(reactor);
    bzm_reactor_config_t config = {
        .engine_count = 4,
        .timestamp_count = 16,
        .lead_zeros = 36,
        .nonce_offset = BZM_NONCE_GAP_1002,
        .enhanced_mode = false,
    };
    TEST_ASSERT_TRUE(bzm_reactor_init(reactor, store, &config,
                                      &SIMULATED_OPS, transport));
    asic_job_t template = bzm_template("incremental");

    for (uint16_t schedule_index = 0; schedule_index < 4;
         ++schedule_index) {
        bzm_work_t work;
        bzm_engine_location_t expected;
        TEST_ASSERT_TRUE(bzm_topology_activation_at(
            schedule_index, BZM_ENGINE_STACK_BOTTOM, &expected));
        TEST_ASSERT_EQUAL(BZM_ASSIGN_OK,
                          bzm_reactor_assign(reactor, &template, &work));
        TEST_ASSERT_EQUAL_UINT16(expected.physical_id, work.engine_id);
        TEST_ASSERT_EQUAL_HEX32(0, work.starting_nonce);
        TEST_ASSERT_EQUAL_HEX32(UINT32_MAX, work.end_nonce);
        TEST_ASSERT_EQUAL_UINT8(0, work.logical_sequence);
    }

    // Physical engine 10 is the second scheduled engine but compact engine
    // 10. Result routing must use the stable compact ID, not schedule index.
    bzm_raw_result_t raw = {
        .asic_id = BZM_FIRST_ASIC_ID,
        .engine_id = 10,
        .status = 8,
        .nonce = 0x100,
        .sequence_id = 0,
        .time = 16,
    };
    bzm_result_t event;
    TEST_ASSERT_TRUE(bzm_reactor_map_result(reactor, &raw, &event));
    TEST_ASSERT_EQUAL_UINT16(10, event.engine_id);

    free(transport);
    free(reactor);
    delete_store(store);
}

TEST_CASE("BZM decoded results own job snapshots after slot retirement",
          "[asic][bzm][result][ownership][qemu-integration]")
{
    bzm_job_store_t *store = new_store();
    simulated_transport_t *transport = new_transport();
    bzm_reactor_t *reactor = new_reactor(store, transport, 2);
    asic_job_t job = bzm_template("snapshot-job");
    TEST_ASSERT_EQUAL(BZM_ASSIGN_OK, bzm_reactor_assign(reactor, &job, NULL));
    bzm_raw_result_t raw = {
        .asic_id = BZM_FIRST_ASIC_ID,
        .engine_id = transport->work[0].engine_id,
        .status = 8, .sequence_id = 1, .time = 15,
    };
    bzm_result_t event;
    TEST_ASSERT_TRUE(bzm_reactor_map_result(reactor, &raw, &event));
    TEST_ASSERT_EQUAL_INT(0, pthread_mutex_trylock(&store->lock));
    TEST_ASSERT_EQUAL_INT(0, pthread_mutex_unlock(&store->lock));
    bzm_job_store_invalidate_all(store);
    memset(&job, 0xa5, sizeof(job));
    TEST_ASSERT_TRUE(event.job_valid);
    TEST_ASSERT_EQUAL_STRING("snapshot-job", event.job.job_id);
    TEST_ASSERT_EQUAL_HEX32(event.job.ntime + 1, event.final_ntime);
    TEST_ASSERT_EQUAL_HEX32(increment_bitmask(event.job.version,
                                              event.job.version_mask),
                             event.final_version);
    bzm_result_t stale;
    TEST_ASSERT_FALSE(bzm_reactor_map_result(reactor, &raw, &stale));
    free(transport);
    free(reactor);
    delete_store(store);
}

TEST_CASE("BZM assigns independent templates and retains one prior job per engine",
          "[asic][bzm][reactor][scheduler][result][qemu-integration]")
{
    bzm_job_store_t *store = new_store();
    simulated_transport_t *transport = new_transport();
    bzm_reactor_t *reactor = new_reactor(store, transport, 2);

    asic_job_t engine0_first = bzm_template("engine0-first");
    asic_job_t engine1_first = bzm_template("engine1-first");
    asic_job_t engine0_next = bzm_template("engine0-next");
    engine0_first.ntime = 100;
    engine1_first.ntime = 200;
    engine0_next.ntime = 300;

    TEST_ASSERT_EQUAL(BZM_ASSIGN_OK,
                      bzm_reactor_assign(reactor, &engine0_first, NULL));
    TEST_ASSERT_EQUAL(BZM_ASSIGN_OK,
                      bzm_reactor_assign(reactor, &engine1_first, NULL));
    TEST_ASSERT_EQUAL(BZM_ASSIGN_OK,
                      bzm_reactor_assign(reactor, &engine0_next, NULL));

    TEST_ASSERT_EQUAL_UINT32(3, transport->write_count);
    TEST_ASSERT_EQUAL_UINT32(3, transport->checkpoint_count);
    TEST_ASSERT_NOT_EQUAL(
        (uint32_t)transport->work[0].source.handle,
        (uint32_t)transport->work[1].source.handle);
    TEST_ASSERT_NOT_EQUAL(
        (uint32_t)transport->work[0].source.handle,
        (uint32_t)transport->work[2].source.handle);
    TEST_ASSERT_EQUAL_UINT8(0, transport->work[0].logical_sequence);
    TEST_ASSERT_EQUAL_UINT8(0, transport->work[1].logical_sequence);
    TEST_ASSERT_EQUAL_UINT8(1, transport->work[2].logical_sequence);
    TEST_ASSERT_EQUAL_HEX32(0, transport->work[2].starting_nonce);
    TEST_ASSERT_EQUAL_HEX32(UINT32_MAX, transport->work[2].end_nonce);

    bzm_raw_result_t raw = {
        .asic_id = BZM_FIRST_ASIC_ID,
        .engine_id = transport->work[0].engine_id,
        .status = 8,
        .sequence_id = 0,
        .time = 16,
    };
    bzm_result_t event;
    TEST_ASSERT_TRUE(bzm_reactor_map_result(reactor, &raw, &event));
    TEST_ASSERT_EQUAL_UINT32(
        (uint32_t)transport->work[0].source.handle,
        (uint32_t)event.work_handle);
    TEST_ASSERT_EQUAL_HEX32(100, event.final_ntime);

    raw.sequence_id = 4;
    TEST_ASSERT_TRUE(bzm_reactor_map_result(reactor, &raw, &event));
    TEST_ASSERT_EQUAL_UINT32(
        (uint32_t)transport->work[2].source.handle,
        (uint32_t)event.work_handle);
    TEST_ASSERT_EQUAL_HEX32(300, event.final_ntime);

    free(transport);
    free(reactor);
    delete_store(store);
}

TEST_CASE("BZM rejects delayed assignments after their bounded store slot is reused",
          "[asic][bzm][reactor][scheduler][result][qemu-integration]")
{
    bzm_job_store_t *store = new_store();
    simulated_transport_t *transport = new_transport();
    bzm_reactor_t *reactor = new_reactor(
        store, transport, BZM_ENGINES_PER_ASIC);
    asic_job_t template = bzm_template("bounded-history");

    for (size_t index = 0; index < BZM_ENGINES_PER_ASIC; ++index) {
        TEST_ASSERT_EQUAL(BZM_ASSIGN_OK,
                          bzm_reactor_assign(reactor, &template, NULL));
    }
    const bzm_work_t first = transport->work[0];

    /* Twenty spare slots retain five seconds of history at the production
     * 250 ms assignment cadence. The next assignment safely retires the
     * oldest handle instead of emitting an event whose template is gone. */
    for (size_t index = 0;
         index <= BZM_JOB_STORE_CAPACITY - BZM_ENGINES_PER_ASIC;
         ++index) {
        TEST_ASSERT_EQUAL(BZM_ASSIGN_OK,
                          bzm_reactor_assign(reactor, &template, NULL));
    }

    bzm_raw_result_t stale = {
        .asic_id = BZM_FIRST_ASIC_ID,
        .engine_id = first.engine_id,
        .status = 8,
        .nonce = 0x1234,
        .sequence_id = 0,
        .time = 16,
    };
    bzm_result_t event;
    TEST_ASSERT_FALSE(bzm_reactor_map_result(reactor, &stale, &event));
    TEST_ASSERT_FALSE(bzm_job_store_contains(store, first.source.handle));

    free(transport);
    free(reactor);
    delete_store(store);
}

TEST_CASE("BZM reactor resolves microstate version and timestamp rolling",
          "[asic][bzm][reactor][result][qemu-integration]")
{
    bzm_job_store_t *store = new_store();
    simulated_transport_t *transport = new_transport();
    bzm_reactor_t *reactor = new_reactor(store, transport, 21);
    asic_job_t template = bzm_template("result");
    template.version_mask = 0x1fffe000;
    TEST_ASSERT_EQUAL(BZM_ASSIGN_OK,
                      bzm_reactor_dispatch(reactor, &template, NULL));

    bzm_raw_result_t raw = {
        .asic_id = 0x1e,
        .engine_id = 64,
        .status = 8,
        .nonce = 0x12345678,
        .sequence_id = 2,
        .time = 13,
        .timestamp_us = 555,
    };
    bzm_result_t event;
    TEST_ASSERT_TRUE(bzm_reactor_map_result(reactor, &raw, &event));
    TEST_ASSERT_EQUAL_UINT8(2, event.asic_index);
    TEST_ASSERT_EQUAL_HEX32(0x2c563412, event.nonce);
    TEST_ASSERT_EQUAL_HEX32(0x20004004,
                            event.final_version);
    TEST_ASSERT_EQUAL_HEX32(0x00004000,
                            event.version_bits);
    TEST_ASSERT_EQUAL_HEX32(template.ntime + 3,
                            event.final_ntime);
    raw.asic_id = 0x1f;
    TEST_ASSERT_FALSE(bzm_reactor_map_result(reactor, &raw, &event));
    TEST_ASSERT_EQUAL_UINT16(20, event.engine_id);
    TEST_ASSERT_EQUAL_UINT8(2, event.micro_job_id);
    TEST_ASSERT_EQUAL_UINT8(0, event.sequence_id);
    TEST_ASSERT_EQUAL_UINT8(2, event.asic_index);

    raw.asic_id = 0x1e;
    raw.nonce++;
    raw.sequence_id = 3;
    raw.time = 12;
    TEST_ASSERT_TRUE(bzm_reactor_map_result(reactor, &raw, &event));
    TEST_ASSERT_EQUAL_HEX32(0x20006004,
                            event.final_version);
    TEST_ASSERT_EQUAL_HEX32(template.ntime + 4,
                            event.final_ntime);

    raw.engine_id = 0;
    raw.sequence_id = 7;
    TEST_ASSERT_FALSE(bzm_reactor_map_result(reactor, &raw, &event));
    raw.sequence_id = 0;
    raw.status = 0;
    TEST_ASSERT_FALSE(bzm_reactor_map_result(reactor, &raw, &event));
    raw.status = 8;
    raw.time = 17;
    TEST_ASSERT_FALSE(bzm_reactor_map_result(reactor, &raw, &event));

    free(transport);
    free(reactor);
    delete_store(store);
}

TEST_CASE("BZM reactor retains both enhanced sequence generations",
          "[asic][bzm][reactor][result][pipeline][qemu-integration]")
{
    bzm_job_store_t *store = new_store();
    simulated_transport_t *transport = new_transport();
    bzm_reactor_t *reactor = new_reactor(store, transport, 2);
    asic_job_t first = bzm_template("first");
    asic_job_t second = bzm_template("second");
    first.ntime = 100;
    second.ntime = 200;

    TEST_ASSERT_EQUAL(BZM_ASSIGN_OK,
                      bzm_reactor_dispatch(reactor, &first, NULL));
    bzm_work_handle_t first_handle = transport->work[0].source.handle;
    TEST_ASSERT_EQUAL(BZM_ASSIGN_OK,
                      bzm_reactor_dispatch(reactor, &second, NULL));
    bzm_work_handle_t second_handle = transport->work[2].source.handle;
    TEST_ASSERT_NOT_EQUAL((uint32_t) first_handle,
                          (uint32_t) second_handle);

    bzm_raw_result_t raw = {
        .asic_id = BZM_FIRST_ASIC_ID,
        .engine_id = transport->work[0].engine_id,
        .status = 8,
        .sequence_id = 0,
        .time = 16,
    };
    bzm_result_t event;
    TEST_ASSERT_TRUE(bzm_reactor_map_result(reactor, &raw, &event));
    TEST_ASSERT_EQUAL_UINT32((uint32_t) first_handle,
                             (uint32_t) event.work_handle);
    TEST_ASSERT_EQUAL_HEX32(100, event.final_ntime);

    raw.sequence_id = 4;
    TEST_ASSERT_TRUE(bzm_reactor_map_result(reactor, &raw, &event));
    TEST_ASSERT_EQUAL_UINT32((uint32_t) second_handle,
                             (uint32_t) event.work_handle);
    TEST_ASSERT_EQUAL_HEX32(200, event.final_ntime);

    TEST_ASSERT_EQUAL(BZM_ASSIGN_FLUSH_REQUIRED,
                      bzm_reactor_dispatch(reactor, &second, NULL));
    TEST_ASSERT_TRUE(bzm_reactor_is_flush_pending(reactor));

    free(transport);
    free(reactor);
    delete_store(store);
}

TEST_CASE("BZM 1002 nonce gap reproduces captured Stage 7 hardware proof",
          "[asic][bzm][reactor][result][hardware-vector][qemu-integration]")
{
    asic_job_t template = {
        .version = 0x20000000,
        .version_mask = 0x1fffe000,
        .ntime = 0x6a5dcd19,
        .nbits = 0x1702369d,
    };
    TEST_ASSERT_EQUAL_UINT32(32, hex2bin(
        "a3617ee3ae390721613cf7d57a0a2c505e2b873f208701000000000000000000",
        template.prev_hash, sizeof(template.prev_hash)));
    TEST_ASSERT_EQUAL_UINT32(32, hex2bin(
        "24f9e61126a07d32f112cfeb01955e0067e577088d6f1b97f1dba64f6c96a113",
        template.merkle_root, sizeof(template.merkle_root)));

    const uint32_t raw_nonce = 0x2d6dac84;
    uint32_t mapped_nonce = __builtin_bswap32(
        raw_nonce - BZM_NONCE_GAP_1002);
    template.ntime += 4;
    double captured_difficulty = mining_nonce_difficulty(
        &template, mapped_nonce, 0x3fff0000);
    TEST_ASSERT_GREATER_THAN_DOUBLE(1.0, captured_difficulty);

    uint32_t former_assumption = __builtin_bswap32(raw_nonce - 0x28U);
    double former_difficulty = mining_nonce_difficulty(
        &template, former_assumption, 0x3fff0000);
    TEST_ASSERT_LESS_THAN_DOUBLE(1.0, former_difficulty);
}

TEST_CASE("BZM distinguishes nonce results from non-share status frames",
          "[asic][bzm][result][qemu-integration]")
{
    bzm_raw_result_t result = {.status = 0x07};
    TEST_ASSERT_FALSE(bzm_raw_result_has_valid_nonce(NULL));
    TEST_ASSERT_FALSE(bzm_raw_result_has_valid_nonce(&result));
    result.status = 0x08;
    TEST_ASSERT_TRUE(bzm_raw_result_has_valid_nonce(&result));
    result.status = 0x0f;
    TEST_ASSERT_TRUE(bzm_raw_result_has_valid_nonce(&result));
}

TEST_CASE("BZM failed flush remains a barrier until transport recovers",
          "[asic][bzm][reactor][flush-error][qemu-integration]")
{
    bzm_job_store_t *store = new_store();
    simulated_transport_t *transport = new_transport();
    bzm_reactor_t *reactor = new_reactor(store, transport, 1);
    asic_job_t template = bzm_template("flush-error");
    TEST_ASSERT_EQUAL(BZM_ASSIGN_OK,
                      bzm_reactor_dispatch(reactor, &template, NULL));

    transport->fail_flush = true;
    reactor->next_sequence = 2;
    TEST_ASSERT_EQUAL(BZM_ASSIGN_TRANSPORT_ERROR,
                      bzm_reactor_dispatch(reactor, &template, NULL));
    TEST_ASSERT_TRUE(bzm_reactor_is_flush_pending(reactor));
    TEST_ASSERT_EQUAL_UINT32(1, transport->flush_count);

    bzm_reactor_finish_flush(reactor);
    TEST_ASSERT_TRUE(bzm_reactor_is_flush_pending(reactor));
    TEST_ASSERT_EQUAL(BZM_ASSIGN_FLUSH_REQUIRED,
                      bzm_reactor_dispatch(reactor, &template, NULL));

    transport->fail_flush = false;
    TEST_ASSERT_TRUE(bzm_reactor_begin_flush(reactor));
    TEST_ASSERT_EQUAL_UINT32(2, transport->flush_count);
    bzm_reactor_finish_flush(reactor);
    TEST_ASSERT_FALSE(bzm_reactor_is_flush_pending(reactor));
    TEST_ASSERT_EQUAL(BZM_ASSIGN_OK,
                      bzm_reactor_dispatch(reactor, &template, NULL));

    free(transport);
    free(reactor);
    delete_store(store);
}

TEST_CASE("BZM clean-job barrier rejects stale results and invalidates handles",
          "[asic][bzm][reactor][clean-job][qemu-integration]")
{
    bzm_job_store_t *store = new_store();
    simulated_transport_t *transport = new_transport();
    bzm_reactor_t *reactor = new_reactor(store, transport, 1);
    asic_job_t template = bzm_template("old");
    TEST_ASSERT_EQUAL(BZM_ASSIGN_OK,
                      bzm_reactor_dispatch(reactor, &template, NULL));
    bzm_work_handle_t old_handle = transport->work[0].source.handle;

    bzm_raw_result_t old_result = {
        .engine_id = 0,
        .status = 8,
        .sequence_id = 0,
        .time = 16,
    };
    bzm_result_t event;
    TEST_ASSERT_TRUE(bzm_reactor_clear_work(reactor));
    TEST_ASSERT_FALSE(bzm_reactor_is_flush_pending(reactor));
    TEST_ASSERT_TRUE(bzm_reactor_results_quarantined(reactor));
    TEST_ASSERT_EQUAL_UINT32(1, transport->flush_count);
    TEST_ASSERT_FALSE(bzm_reactor_map_result(reactor, &old_result, &event));
    asic_job_t snapshot;
    TEST_ASSERT_FALSE(bzm_job_store_snapshot(store, old_handle, &snapshot));

    template = bzm_template("new");
    TEST_ASSERT_EQUAL(BZM_ASSIGN_OK,
                      bzm_reactor_dispatch(reactor, &template, NULL));
    TEST_ASSERT_FALSE(bzm_reactor_results_quarantined(reactor));
    bzm_work_handle_t new_handle =
        transport->work[transport->write_count - 1].source.handle;
    TEST_ASSERT_NOT_EQUAL((uint32_t)old_handle, (uint32_t)new_handle);
    TEST_ASSERT_FALSE(bzm_job_store_snapshot(store, old_handle, &snapshot));

    free(transport);
    free(reactor);
    delete_store(store);
}

TEST_CASE("BZM idle clean-job barrier does not disturb the hardware link",
          "[asic][bzm][reactor][clean-job][idle][qemu-integration]")
{
    bzm_job_store_t *store = new_store();
    simulated_transport_t *transport = new_transport();
    bzm_reactor_t *reactor = new_reactor(store, transport, 1);
    asic_job_t template = bzm_template("queued-only");
    bzm_work_handle_t handle = BZM_WORK_HANDLE_INVALID;
    TEST_ASSERT_TRUE(bzm_job_store_store_generated(store, &template,
                                                     &handle));

    TEST_ASSERT_TRUE(bzm_reactor_clear_work(reactor));
    TEST_ASSERT_EQUAL_UINT32(0, transport->flush_count);
    TEST_ASSERT_FALSE(bzm_reactor_is_flush_pending(reactor));
    TEST_ASSERT_TRUE(bzm_reactor_results_quarantined(reactor));
    asic_job_t snapshot;
    TEST_ASSERT_FALSE(bzm_job_store_snapshot(store, handle, &snapshot));

    free(transport);
    free(reactor);
    delete_store(store);
}

TEST_CASE("BZM quarantines clean-job results through the full engine rotation",
          "[asic][bzm][reactor][clean-job][scheduler][qemu-integration]")
{
    bzm_job_store_t *store = new_store();
    simulated_transport_t *transport = new_transport();
    bzm_reactor_t *reactor = new_reactor(store, transport, 2);
    asic_job_t template = bzm_template("replacement");

    TEST_ASSERT_TRUE(bzm_reactor_clear_work(reactor));
    TEST_ASSERT_TRUE(bzm_reactor_results_quarantined(reactor));
    TEST_ASSERT_EQUAL(BZM_ASSIGN_OK,
                      bzm_reactor_assign(reactor, &template, NULL));
    TEST_ASSERT_TRUE(bzm_reactor_results_quarantined(reactor));
    TEST_ASSERT_EQUAL(BZM_ASSIGN_OK,
                      bzm_reactor_assign(reactor, &template, NULL));
    TEST_ASSERT_FALSE(bzm_reactor_results_quarantined(reactor));

    free(transport);
    free(reactor);
    delete_store(store);
}

TEST_CASE("BZM repeated clean refreshes preserve frequency replacement proof",
          "[asic][bzm][reactor][clean-job][evidence][qemu-integration]")
{
    /* Reproduce the PR #5 HIL cadence with the corrected production policy.
     * Each notification arrives before a 236-engine rotation can finish.
     * Logical time models the pool's two-second refresh; transport timing and
     * nonce validity are not emulated here. Non-clean refreshes are the control.
     */
    for (unsigned repeated_clean = 0; repeated_clean <= 1; ++repeated_clean) {
        bzm_job_store_t *store = new_store();
        simulated_transport_t *transport = new_transport();
        bzm_reactor_t *reactor = new_reactor(
            store, transport, BZM_ENGINES_PER_ASIC);
        asic_job_t template = bzm_template("refresh-proof");
        bzm_running_stats_t baseline = {0};
        bzm_running_stats_t current = {
            /* Valid results can precede the first clean replacement, as in
             * the captured run. They cannot satisfy the batch requirement. */
            .mapped_results = 85,
            .locally_valid_results = 85,
        };
        bzm_running_evidence_config_t config = {
            .required_chip_engine_writes = 944,
            .minimum_valid_results = 1,
            .allow_mapping_recovery = true,
            .maximum_mapping_rejections = 16,
            .maximum_local_rejections = 16,
            .proof_timeout_ms = 90000,
            .recovery_timeout_ms = 30000,
        };
        bzm_running_evidence_lifecycle_t lifecycle = {.completed_once = true};
        bzm_running_evidence_result_t evidence = bzm_running_evidence_track(
            &lifecycle, &baseline, &current, &config, 0, 0);
        TEST_ASSERT_EQUAL(BZM_RUNNING_EVIDENCE_GOOD, evidence.status);

        for (unsigned refresh = 0; refresh < 15; ++refresh) {
            if (refresh == 0 || repeated_clean)
                TEST_ASSERT_TRUE(bzm_reactor_invalidate_work(reactor));
            transport->write_count = 0; /* Bounded capture for this refresh. */
            for (unsigned engine = 0; engine < 25; ++engine) {
                TEST_ASSERT_EQUAL(BZM_ASSIGN_OK,
                                  bzm_reactor_assign(reactor, &template, NULL));
                current.dispatched_logical_engines++;
                current.dispatched_chip_engines += 4;
                if (reactor->next_engine == 0)
                    current.dispatch_batches++;
                TEST_ASSERT_FALSE(bzm_reactor_results_quarantined(reactor));
            }
            if (!bzm_reactor_results_quarantined(reactor)) {
                current.mapped_results++;
                current.locally_valid_results++;
            }
            evidence = bzm_running_evidence_track(
                &lifecycle, &baseline, &current, &config, 0,
                (refresh + 1) * 2000U);
        }
        TEST_ASSERT_GREATER_THAN_UINT32(944, current.dispatched_chip_engines);
        TEST_ASSERT_GREATER_THAN_UINT32(0, current.dispatch_batches);
        TEST_ASSERT_FALSE(bzm_reactor_results_quarantined(reactor));
        TEST_ASSERT_EQUAL(BZM_RUNNING_EVIDENCE_GOOD, evidence.status);

        free(transport);
        free(reactor);
        delete_store(store);
    }
}

TEST_CASE("BZM clean jobs retire delayed results without resetting engine order",
          "[asic][bzm][reactor][clean-job][stale][qemu-integration]")
{
    bzm_job_store_t *store = new_store();
    simulated_transport_t *transport = new_transport();
    bzm_reactor_t *reactor = new_reactor(store, transport, 2);
    asic_job_t template = bzm_template("old");
    bzm_work_t old_work, new_work;
    TEST_ASSERT_EQUAL(BZM_ASSIGN_OK, bzm_reactor_assign(reactor, &template, &old_work));
    bzm_raw_result_t old = {
        .asic_id = BZM_FIRST_ASIC_ID, .engine_id = old_work.engine_id,
        .sequence_id = old_work.logical_sequence << 2, .time = 16, .status = 8,
    };
    bzm_result_t event;
    TEST_ASSERT_TRUE(bzm_reactor_map_result(reactor, &old, &event));
    TEST_ASSERT_TRUE(bzm_reactor_invalidate_work(reactor));
    TEST_ASSERT_EQUAL_UINT16(1, reactor->next_engine);
    TEST_ASSERT_EQUAL_UINT32(0, transport->flush_count);
    TEST_ASSERT_FALSE(bzm_reactor_map_result(reactor, &old, &event));
    TEST_ASSERT_TRUE(bzm_reactor_result_is_stale(reactor, &old));
    TEST_ASSERT_FALSE(bzm_job_store_contains(store, old_work.source.handle));

    // A second clean notification must not restart the same engine again.
    TEST_ASSERT_TRUE(bzm_reactor_invalidate_work(reactor));
    TEST_ASSERT_EQUAL(BZM_ASSIGN_OK, bzm_reactor_assign(reactor, &template, &new_work));
    TEST_ASSERT_NOT_EQUAL(old_work.engine_id, new_work.engine_id);
    bzm_raw_result_t fresh = old;
    fresh.engine_id = new_work.engine_id;
    fresh.sequence_id = new_work.logical_sequence << 2;
    TEST_ASSERT_TRUE(bzm_reactor_map_result(reactor, &fresh, &event));
    TEST_ASSERT_TRUE(bzm_reactor_result_is_stale(reactor, &old));

    // When the old engine is reached, its sequence advances across invalidation.
    TEST_ASSERT_EQUAL(BZM_ASSIGN_OK, bzm_reactor_assign(reactor, &template, &new_work));
    TEST_ASSERT_EQUAL_UINT16(old_work.engine_id, new_work.engine_id);
    TEST_ASSERT_NOT_EQUAL(old_work.logical_sequence, new_work.logical_sequence);
    TEST_ASSERT_FALSE(bzm_reactor_map_result(reactor, &old, &event));
    TEST_ASSERT_TRUE(bzm_reactor_result_is_stale(reactor, &old));
    fresh = old;
    fresh.sequence_id = new_work.logical_sequence << 2;
    TEST_ASSERT_TRUE(bzm_reactor_map_result(reactor, &fresh, &event));
    fresh.sequence_id = 40 << 2; // Never issued: remains a mapping error.
    TEST_ASSERT_FALSE(bzm_reactor_map_result(reactor, &fresh, &event));
    TEST_ASSERT_FALSE(bzm_reactor_result_is_stale(reactor, &fresh));

    free(transport);
    free(reactor);
    delete_store(store);
}

TEST_CASE("BZM incremental sequence reuse requires a hardware barrier",
          "[asic][bzm][reactor][wrap][clean-job][qemu-integration]")
{
    bzm_job_store_t *store = new_store();
    simulated_transport_t *transport = new_transport();
    bzm_reactor_t *reactor = new_reactor(store, transport, 2);
    asic_job_t template = bzm_template("wrap");
    for (unsigned rotation = 0; rotation < 63; ++rotation) {
        TEST_ASSERT_TRUE(bzm_reactor_invalidate_work(reactor));
        for (unsigned engine = 0; engine < 2; ++engine) {
            bzm_work_t work;
            TEST_ASSERT_EQUAL(BZM_ASSIGN_OK, bzm_reactor_assign(reactor, &template, &work));
            TEST_ASSERT_EQUAL_UINT8(rotation, work.logical_sequence);
        }
    }
    size_t writes = transport->write_count;
    TEST_ASSERT_EQUAL(BZM_ASSIGN_FLUSH_REQUIRED, bzm_reactor_assign(reactor, &template, NULL));
    TEST_ASSERT_EQUAL_UINT32(writes, transport->write_count);
    transport->fail_flush = true;
    TEST_ASSERT_FALSE(bzm_reactor_begin_flush(reactor));
    bzm_reactor_finish_flush(reactor);
    TEST_ASSERT_EQUAL(BZM_ASSIGN_FLUSH_REQUIRED, bzm_reactor_assign(reactor, &template, NULL));
    transport->fail_flush = false;
    TEST_ASSERT_TRUE(bzm_reactor_begin_flush(reactor));
    bzm_reactor_finish_flush(reactor);
    for (unsigned engine = 0; engine < 2; ++engine) {
        // A clean burst during the reuse barrier preserves its progress.
        TEST_ASSERT_TRUE(bzm_reactor_invalidate_work(reactor));
        TEST_ASSERT_TRUE(bzm_reactor_results_quarantined(reactor));
        bzm_work_t work;
        TEST_ASSERT_EQUAL(BZM_ASSIGN_OK, bzm_reactor_assign(reactor, &template, &work));
        TEST_ASSERT_EQUAL_UINT8(0, work.logical_sequence);
        TEST_ASSERT_EQUAL(engine == 0, bzm_reactor_results_quarantined(reactor));
    }

    free(transport);
    free(reactor);
    delete_store(store);
}

TEST_CASE("BZM sequence wrap forces a flush before identity reuse",
          "[asic][bzm][reactor][wrap][qemu-integration]")
{
    bzm_job_store_t *store = new_store();
    simulated_transport_t *transport = new_transport();
    bzm_reactor_t *reactor = new_reactor(store, transport, 1);

    for (size_t i = 0; i < 2; ++i) {
        asic_job_t template = bzm_template("wrap");
        TEST_ASSERT_EQUAL(BZM_ASSIGN_OK,
                          bzm_reactor_dispatch(reactor, &template, NULL));

    }
    asic_job_t template = bzm_template("wrapped");
    TEST_ASSERT_EQUAL(BZM_ASSIGN_FLUSH_REQUIRED,
                      bzm_reactor_dispatch(reactor, &template, NULL));
    TEST_ASSERT_TRUE(bzm_reactor_is_flush_pending(reactor));
    TEST_ASSERT_EQUAL_UINT32(1, transport->flush_count);

    bzm_raw_result_t stale = {
        .engine_id = 0,
        .status = 8,
        .sequence_id = 0,
        .time = 16,
    };
    bzm_result_t event;
    TEST_ASSERT_FALSE(bzm_reactor_map_result(reactor, &stale, &event));
    bzm_reactor_finish_flush(reactor);
    TEST_ASSERT_EQUAL(BZM_ASSIGN_OK,
                      bzm_reactor_dispatch(reactor, &template, NULL));
    TEST_ASSERT_EQUAL_UINT8(0,
        transport->work[transport->write_count - 1].logical_sequence);

    free(transport);
    free(reactor);
    delete_store(store);
}

TEST_CASE("BZM partial dispatch is flushed and never publishes a handle",
          "[asic][bzm][reactor][error][qemu-integration]")
{
    bzm_job_store_t *store = new_store();
    simulated_transport_t *transport = new_transport();
    transport->fail_after = 1;
    bzm_reactor_t *reactor = new_reactor(store, transport, 2);
    asic_job_t template = bzm_template("partial");
    size_t assigned;
    TEST_ASSERT_EQUAL(BZM_ASSIGN_TRANSPORT_ERROR,
                      bzm_reactor_dispatch(reactor, &template, &assigned));
    TEST_ASSERT_EQUAL_UINT32(0, assigned);
    TEST_ASSERT_EQUAL_UINT32(1, transport->flush_count);
    TEST_ASSERT_FALSE(bzm_reactor_is_flush_pending(reactor));
    asic_job_t snapshot;
    TEST_ASSERT_FALSE(bzm_job_store_snapshot(
        store, transport->work[0].source.handle, &snapshot));
    free(transport);
    free(reactor);

    simulated_transport_t *first_write_failure = new_transport();
    first_write_failure->fail_immediately = true;
    bzm_reactor_t *first_write_reactor = new_reactor(
        store, first_write_failure, 2);
    assigned = 99;
    TEST_ASSERT_EQUAL(BZM_ASSIGN_TRANSPORT_ERROR,
                      bzm_reactor_dispatch(first_write_reactor, &template,
                                           &assigned));
    TEST_ASSERT_EQUAL_UINT32(0, assigned);
    TEST_ASSERT_EQUAL_UINT32(1, first_write_failure->flush_count);
    TEST_ASSERT_FALSE(bzm_reactor_is_flush_pending(first_write_reactor));
    free(first_write_failure);
    free(first_write_reactor);

    simulated_transport_t *checkpoint_failure = new_transport();
    checkpoint_failure->fail_checkpoint = true;
    bzm_reactor_t *checkpoint_reactor = new_reactor(
        store, checkpoint_failure, 2);
    assigned = 99;
    TEST_ASSERT_EQUAL(BZM_ASSIGN_TRANSPORT_ERROR,
                      bzm_reactor_dispatch(checkpoint_reactor, &template,
                                           &assigned));
    TEST_ASSERT_EQUAL_UINT32(0, assigned);
    TEST_ASSERT_EQUAL_UINT32(1, checkpoint_failure->write_count);
    TEST_ASSERT_EQUAL_UINT32(1, checkpoint_failure->checkpoint_count);
    TEST_ASSERT_EQUAL_UINT32(1, checkpoint_failure->flush_count);
    TEST_ASSERT_FALSE(bzm_reactor_is_flush_pending(checkpoint_reactor));
    free(checkpoint_failure);
    free(checkpoint_reactor);

    simulated_transport_t *transport2 = new_transport();
    bzm_reactor_t *invalid = calloc(1, sizeof(*invalid));
    TEST_ASSERT_NOT_NULL(invalid);
    bzm_reactor_config_t bad = {
        .engine_count = BZM_ENGINES_PER_ASIC + 1,
        .timestamp_count = 16,
        .enhanced_mode = true,
    };
    TEST_ASSERT_FALSE(bzm_reactor_init(invalid, store, &bad,
                                       &SIMULATED_OPS, transport2));

    free(transport2);
    free(invalid);
    delete_store(store);
}
