#ifndef BITTOOLS_FLIPPERS_H
#define BITTOOLS_FLIPPERS_H

#include <stddef.h>
#include <stdint.h>

void flip80bytes(void *dest_p, const void *src_p);

void flip32bytes(void *dest_p, const void *src_p);

uint32_t flip32(uint32_t val);

void swap_endian_words(const char * hex,  uint8_t * output);

void reverse_bytes(uint8_t * data, size_t len);

uint8_t reverse_bits(uint8_t num) {
    uint8_t reversed = 0;
    int i;

    for (i = 0; i < 8; i++) {
        reversed <<= 1;     // Left shift the reversed variable by 1
        reversed |= num & 1; // Use bitwise OR to set the rightmost bit of reversed to the current bit of num
        num >>= 1;          // Right shift num by 1 to get the next bit
    }

    return reversed;
}

#endif // BITTOOLS_FLIPPERS_H