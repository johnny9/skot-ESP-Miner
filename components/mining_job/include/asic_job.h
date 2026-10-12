#ifndef ASIC_JOB_H_
#define ASIC_JOB_H_

#include <stdint.h>

#define ASIC_JOB_ID_LEN 32
#define ASIC_JOB_EXTRANONCE2_SIZE 32
#define ASIC_JOB_EXTRANONCE2_HEX_SIZE (ASIC_JOB_EXTRANONCE2_SIZE * 2 + 1)

typedef enum {
    JOB_TYPE_V1 = 0,
    JOB_TYPE_SV2_STANDARD,
    JOB_TYPE_SV2_EXTENDED,
} mining_job_source_t;

/* Hash arrays use Bitcoin header byte order; integers use host byte order.
 * Metadata strings must be null-terminated. */
typedef struct asic_job {
    uint32_t version;
    uint32_t version_mask;
    uint8_t prev_hash[32];
    uint8_t merkle_root[32];
    uint32_t ntime;
    uint32_t nbits;
    uint32_t starting_nonce;
    uint8_t pool_target[32];
    uint8_t pool_id;
    mining_job_source_t source_type;
    char job_id[ASIC_JOB_ID_LEN];
    char extranonce2[ASIC_JOB_EXTRANONCE2_HEX_SIZE];
} asic_job_t;

/* Writes an 80-byte Bitcoin header with little-endian integers. */
void asic_job_header(const asic_job_t *job, uint32_t nonce,
                     uint32_t version, uint8_t header[80]);

// BIP320 bits 13..28 supported by BM13xx version rolling.
#define BIP320_VERSION_ROLLING_MASK 0x1fffe000U
uint32_t increment_bitmask(uint32_t value, uint32_t mask);
// Accepts unaligned buffers; source and destination must not overlap.
void reverse_32bit_words(const uint8_t src[32], uint8_t dest[32]);

void reverse_endianness_per_word(uint8_t data[32]);

#endif
