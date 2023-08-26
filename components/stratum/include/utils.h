#ifndef STRATUM_UTILS_H
#define STRATUM_UTILS_H

char * double_sha256(const char * hex_string);

uint8_t * double_sha256_bin(const uint8_t * data, const size_t data_len);

void single_sha256_bin(const uint8_t * data, const size_t data_len, uint8_t * dest);

void midstate_sha256_bin( const uint8_t * data, const size_t data_len, uint8_t * dest);

double le256todouble(const void *target);

#endif // STRATUM_UTILS_H