#ifndef BITTOOLS_HEX_H
#define BITTOOLS_HEX_H

#include <stddef.h>
#include <stdint.h>

int hex2char(uint8_t x, char * c);

uint8_t hex2val(char c);

size_t hex2bin(const char * hex, uint8_t * bin, size_t bin_len);

size_t bin2hex(const uint8_t * buf, size_t buflen, char * hex, size_t hexlen);

void print_hex(const uint8_t * b, size_t len,
               const size_t in_line, const char * prefix);

void prettyHex(unsigned char * buf, int len);

#endif