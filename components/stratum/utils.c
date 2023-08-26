#include "utils.h"

#include <string.h>
#include <stdio.h>

#include "mbedtls/sha256.h"

char * double_sha256(const char * hex_string)
{
    size_t bin_len = strlen(hex_string) / 2;
    uint8_t * bin = malloc(bin_len);
    hex2bin(hex_string, bin, bin_len);

    unsigned char first_hash_output[32], second_hash_output[32];

    mbedtls_sha256(bin, bin_len, first_hash_output, 0);
    mbedtls_sha256(first_hash_output, 32, second_hash_output, 0);

    free(bin);

    char * output_hash = malloc(64 + 1);
    bin2hex(second_hash_output, 32, output_hash, 65);
    return output_hash;
}

uint8_t * double_sha256_bin(const uint8_t * data, const size_t data_len)
{
    uint8_t first_hash_output[32];
    uint8_t * second_hash_output = malloc(32);

    mbedtls_sha256(data, data_len, first_hash_output, 0);
    mbedtls_sha256(first_hash_output, 32, second_hash_output, 0);

    return second_hash_output;
}

void single_sha256_bin( const uint8_t * data, const size_t data_len, uint8_t * dest)
{
    //mbedtls_sha256(data, data_len, dest, 0);

    // Initialize SHA256 context
    mbedtls_sha256_context sha256_ctx;
    mbedtls_sha256_init(&sha256_ctx);
    mbedtls_sha256_starts(&sha256_ctx, 0);

    // Compute first SHA256 hash of header
    mbedtls_sha256_update(&sha256_ctx, data, 64);
    unsigned char hash[32];
    mbedtls_sha256_finish(&sha256_ctx, hash);

    // Compute midstate from hash
    memcpy(dest, hash, 32);

}

void midstate_sha256_bin( const uint8_t * data, const size_t data_len, uint8_t * dest)
{
    mbedtls_sha256_context midstate;

    //Calculate midstate
    mbedtls_sha256_init(&midstate);
    mbedtls_sha256_starts(&midstate, 0);
    mbedtls_sha256_update(&midstate, data, 64);

    //memcpy(dest, midstate.state, 32);
    flip32bytes(dest, midstate.state);

}

void swap_endian_words(const char * hex_words, uint8_t * output) {
    size_t hex_length = strlen(hex_words);
    if (hex_length % 8 != 0) {
        fprintf(stderr, "Must be 4-byte word aligned\n");
        exit(EXIT_FAILURE);
    }

    size_t binary_length = hex_length / 2;

    for (size_t i = 0; i < binary_length; i += 4) {
        for (int j = 0; j < 4; j++) {
            unsigned int byte_val;
            sscanf(hex_words + (i + j) * 2, "%2x", &byte_val);
            output[i + (3 - j)] = byte_val;
        }
    }
}

void reverse_bytes(uint8_t * data, size_t len) {
    for (int i = 0; i < len / 2; ++i) {
        uint8_t temp = data[i];
        data[i] = data[len - 1 - i];
        data[len - 1 - i] = temp;
    }
}

//static const double truediffone = 26959535291011309493156476344723991336010898738574164086137773096960.0;
static const double bits192 = 6277101735386680763835789423207666416102355444464034512896.0;
static const double bits128 = 340282366920938463463374607431768211456.0;
static const double bits64 = 18446744073709551616.0;

/* Converts a little endian 256 bit value to a double */
double le256todouble(const void *target)
{
	uint64_t *data64;
	double dcut64;

	data64 = (uint64_t *)(target + 24);
	dcut64 = *data64 * bits192;

	data64 = (uint64_t *)(target + 16);
	dcut64 += *data64 * bits128;

	data64 = (uint64_t *)(target + 8);
	dcut64 += *data64 * bits64;

	data64 = (uint64_t *)(target);
	dcut64 += *data64;

	return dcut64;
}

void prettyHex(unsigned char * buf, int len) {
    int i;
    printf("[");
    for (i = 0; i < len-1; i++) {
        printf("%02X ", buf[i]);
    }
    printf("%02X]\n", buf[len-1]);
}

uint32_t flip32(uint32_t val) {
    uint32_t ret = 0;
    ret |= (val & 0xFF) << 24;
    ret |= (val & 0xFF00) << 8;
    ret |= (val & 0xFF0000) >> 8;
    ret |= (val & 0xFF000000) >> 24;
    return ret;
}