#ifndef ASIC_ERR_H
#define ASIC_ERR_H

/* Values preserve ESP-IDF's generic error ABI at the platform boundary. */
typedef int asic_err_t;
enum {
    ASIC_OK = 0, ASIC_FAIL = -1,
    ASIC_ERR_NO_MEM = 0x101, ASIC_ERR_INVALID_ARG = 0x102,
    ASIC_ERR_INVALID_STATE = 0x103, ASIC_ERR_INVALID_SIZE = 0x104,
    ASIC_ERR_NOT_FOUND = 0x105, ASIC_ERR_NOT_SUPPORTED = 0x106,
    ASIC_ERR_TIMEOUT = 0x107, ASIC_ERR_INVALID_RESPONSE = 0x108,
    ASIC_ERR_INVALID_CRC = 0x109,
};
const char *asic_error_name(asic_err_t error);

#endif
