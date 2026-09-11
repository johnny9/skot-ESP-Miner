#include "unity.h"
#include "mining_fixture_bindings.h"
#include "mining.h"
#include "utils.h"

#include <string.h>

static void assert_hash(const char *hex, const uint8_t actual[32])
{
    uint8_t expected[32];
    TEST_ASSERT_EQUAL_UINT32(32, hex2bin(hex, expected, sizeof(expected)));
    TEST_ASSERT_EQUAL_HEX8_ARRAY(expected, actual, sizeof(expected));
}

TEST_CASE("coinbase hash keeps stack and heap boundaries byte exact",
          "[mining][job-building]")
{
    static uint8_t bytes[1025];
    uint8_t hash[32];
    memset(bytes, 0xa5, sizeof(bytes));

    mining_allocator_fixture_reset(0);
    calculate_coinbase_tx_hash_bin(bytes, 1024, NULL, 0, NULL, 0, NULL, 0, hash);
    TEST_ASSERT_EQUAL_UINT32(0, mining_allocator_fixture_calls());
    assert_hash("68340791fcd0a423a34a6bbf7794ccf983f0290d46c2c74990cde7e14c1b2dea", hash);

    calculate_coinbase_tx_hash_bin(bytes, 1025, NULL, 0, NULL, 0, NULL, 0, hash);
    TEST_ASSERT_EQUAL_UINT32(1, mining_allocator_fixture_calls());
    TEST_ASSERT_EQUAL_UINT32(1025, mining_allocator_fixture_last_size());
    assert_hash("6456e426406ca41a386e3ff3fa13b02fb4ef606512de6deea88a42333dc105c4", hash);

    // Four non-empty segments produce exactly the same 1,025-byte message.
    calculate_coinbase_tx_hash_bin(bytes, 1000, bytes, 8, bytes, 8, bytes, 9, hash);
    TEST_ASSERT_EQUAL_UINT32(2, mining_allocator_fixture_calls());
    assert_hash("6456e426406ca41a386e3ff3fa13b02fb4ef606512de6deea88a42333dc105c4", hash);
}

TEST_CASE("empty coinbase segments accept null and nonnull zero-length buffers",
          "[mining][job-building]")
{
    uint8_t byte = 0xa5;
    uint8_t hash[32];
    mining_allocator_fixture_reset(0);
    calculate_coinbase_tx_hash_bin(NULL, 0, NULL, 0, NULL, 0, NULL, 0, hash);
    assert_hash("5df6e0e2761359d30a8275058e299fcc0381534545f55cf43e41983f5d4c9456", hash);
    calculate_coinbase_tx_hash_bin(&byte, 0, &byte, 0, &byte, 0, &byte, 0, hash);
    assert_hash("5df6e0e2761359d30a8275058e299fcc0381534545f55cf43e41983f5d4c9456", hash);
    TEST_ASSERT_EQUAL_UINT32(0, mining_allocator_fixture_calls());
}

TEST_CASE("coinbase allocation failure preserves zero output and next-call recovery",
          "[mining][job-building]")
{
    static uint8_t bytes[1025];
    uint8_t hash[32];
    memset(bytes, 0xa5, sizeof(bytes));
    memset(hash, 0xff, sizeof(hash));

    mining_allocator_fixture_reset(1);
    calculate_coinbase_tx_hash_bin(bytes, sizeof(bytes), NULL, 0, NULL, 0, NULL, 0, hash);
    TEST_ASSERT_EQUAL_UINT32(1, mining_allocator_fixture_calls());
    assert_hash("0000000000000000000000000000000000000000000000000000000000000000", hash);
    calculate_coinbase_tx_hash_bin(bytes, sizeof(bytes), NULL, 0, NULL, 0, NULL, 0, hash);
    TEST_ASSERT_EQUAL_UINT32(2, mining_allocator_fixture_calls());
    assert_hash("6456e426406ca41a386e3ff3fa13b02fb4ef606512de6deea88a42333dc105c4", hash);

    // The allocation-failure guard also permits no output buffer.
    mining_allocator_fixture_reset(1);
    calculate_coinbase_tx_hash_bin(bytes, sizeof(bytes), NULL, 0, NULL, 0, NULL, 0, NULL);
    TEST_ASSERT_EQUAL_UINT32(1, mining_allocator_fixture_calls());
    mining_allocator_fixture_reset(0);
}

TEST_CASE("Bitmain job defaults and explicit values preserve metadata precedence",
          "[mining][job-building]")
{
    miner_job_t source = {
        .version = 0x20000000,
        .version_mask = 0,
        .pool_diff = 0,
        .nbits = 0x1705dd01,
        .ntime = 0x64658bd8,
        .pool_id = 3,
        .type = JOB_TYPE_SV2_STANDARD,
    };
    uint8_t merkle[32] = {0};
    bm_job result = {0};
    construct_bm_job_from_miner_job(&source, 0, merkle, 0x0000e000, 17.0, 0, &result);
    TEST_ASSERT_EQUAL_HEX32(source.version, result.version);
    TEST_ASSERT_EQUAL_HEX32(0x0000e000, result.version_mask);
    TEST_ASSERT_EQUAL_DOUBLE(17.0, result.pool_diff);
    TEST_ASSERT_EQUAL_UINT8(0, result.num_midstates);
    TEST_ASSERT_EQUAL_HEX32(source.nbits, result.target);
    TEST_ASSERT_EQUAL_HEX32(source.ntime, result.ntime);
    TEST_ASSERT_EQUAL_UINT8(source.pool_id, result.pool_id);
    TEST_ASSERT_EQUAL_INT(source.type, result.job_type);
    TEST_ASSERT_EQUAL_UINT32(0, result.starting_nonce);

    source.version_mask = 0x1fffe000;
    source.pool_diff = 512.0;
    construct_bm_job_from_miner_job(&source, 0x20002000, merkle, 0x0000e000, 17.0, 0, &result);
    TEST_ASSERT_EQUAL_HEX32(0x20002000, result.version);
    TEST_ASSERT_EQUAL_HEX32(source.version_mask, result.version_mask);
    TEST_ASSERT_EQUAL_DOUBLE(source.pool_diff, result.pool_diff);
}

TEST_CASE("Bitmain software midstate count honors zero mask and the buffer limit",
          "[mining][job-building]")
{
    miner_job_t source = { .version = 0x20000000 };
    uint8_t merkle[32] = {0};
    bm_job first = {0}, limited = {0};

    construct_bm_job_from_miner_job(&source, 0, merkle, 0, 1.0, 4, &first);
    TEST_ASSERT_EQUAL_UINT8(1, first.num_midstates);
    construct_bm_job_from_miner_job(&source, 0, merkle, 0x1fffe000, 1.0,
                                   BM_JOB_MAX_MIDSTATES, &first);
    construct_bm_job_from_miner_job(&source, 0, merkle, 0x1fffe000, 1.0, UINT8_MAX, &limited);
    TEST_ASSERT_EQUAL_UINT8(BM_JOB_MAX_MIDSTATES, first.num_midstates);
    TEST_ASSERT_EQUAL_UINT8(BM_JOB_MAX_MIDSTATES, limited.num_midstates);
    TEST_ASSERT_EQUAL_HEX8_ARRAY(first.midstates, limited.midstates, sizeof(first.midstates));
}

TEST_CASE("version increments preserve zero masks and existing sparse carry behavior",
          "[mining][job-building]")
{
    TEST_ASSERT_EQUAL_HEX32(0x20002000, increment_bitmask(0x20002000, 0));
    TEST_ASSERT_EQUAL_HEX32(0x20002000, increment_bitmask(0x20000000, 0x00006000));
    TEST_ASSERT_EQUAL_HEX32(0x20004000, increment_bitmask(0x20002000, 0x00006000));
    TEST_ASSERT_EQUAL_HEX32(0x20000004, increment_bitmask(0x20000001, 0x00000005));
    TEST_ASSERT_EQUAL_HEX32(0, increment_bitmask(UINT32_MAX, UINT32_MAX));
}
