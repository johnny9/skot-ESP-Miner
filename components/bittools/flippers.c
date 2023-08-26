#include "bittools/flippers.h"

#ifndef bswap_16
#define bswap_16(a) ((((uint16_t) (a) << 8) & 0xff00) | (((uint16_t) (a) >> 8) & 0xff))
#endif

#ifndef bswap_32
#define bswap_32(a) ((((uint32_t) (a) << 24) & 0xff000000) | \
		     (((uint32_t) (a) << 8) & 0xff0000) | \
             (((uint32_t) (a) >> 8) & 0xff00) | \
             (((uint32_t) (a) >> 24) & 0xff))
#endif

uint8_t reverse_bits(uint8_t num) {
    uint8_t reversed = 0;
    int i;

    for (i = 0; i < 8; i++) {
        reversed <<= 1;
        reversed |= num & 1;
        num >>= 1;
    }

    return reversed;
}




/*
 * General byte order swapping functions.
 */
#define	bswap16(x)	__bswap16(x)
#define	bswap32(x)	__bswap32(x)
#define	bswap64(x)	__bswap64(x)

uint32_t swab32(uint32_t v) {
    return bswap_32(v);
}

//takes 80 bytes and flips every 4 bytes
void flip80bytes(void *dest_p, const void *src_p) {
	uint32_t *dest = dest_p;
	const uint32_t *src = src_p;
	int i;

	for (i = 0; i < 20; i++)
		dest[i] = swab32(src[i]);
}

void flip64bytes(void *dest_p, const void *src_p) {
    uint32_t *dest = dest_p;
    const uint32_t *src = src_p;
    int i;

    for (i = 0; i < 16; i++)
        dest[i] = swab32(src[i]);
}

void flip32bytes(void *dest_p, const void *src_p) {
    uint32_t *dest = dest_p;
    const uint32_t *src = src_p;
    int i;

    for (i = 0; i < 8; i++)
        dest[i] = swab32(src[i]);
}