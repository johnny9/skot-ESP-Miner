#ifndef MINING_FIXTURE_BINDINGS_H
#define MINING_FIXTURE_BINDINGS_H

#include <stdlib.h>
#include "mining_allocator_fixture.h"

/* The host instruments the original production files directly. ESP-IDF
 * compiles the same source into a private fixture so unrelated tests and
 * system tasks cannot consume an injected allocation failure. */
#ifdef ESP_PLATFORM
#define free_bm_job fixture_free_bm_job
#define calculate_coinbase_tx_hash_bin fixture_calculate_coinbase_tx_hash_bin
#define calculate_merkle_root_hash fixture_calculate_merkle_root_hash
#define construct_bm_job_from_miner_job fixture_construct_bm_job_from_miner_job
#define hash_to_pdiff fixture_hash_to_pdiff
#define test_nonce_value fixture_test_nonce_value
#define increment_bitmask fixture_increment_bitmask
#endif

#define malloc(size) mining_allocator_fixture_malloc(size)

#endif
