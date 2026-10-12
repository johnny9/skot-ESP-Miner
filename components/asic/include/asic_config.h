#ifndef ASIC_CONFIG_H
#define ASIC_CONFIG_H
#include <stdint.h>
#include "asic_job.h"

typedef enum { BM1397, BM1366, BM1368, BM1370, BM1373 } asic_model_t;
typedef struct {
    asic_model_t model;
    uint16_t asic_count;
    uint16_t core_count;
    uint16_t small_core_count;
    uint16_t difficulty;
    uint16_t default_timeout_ms;
    uint8_t software_midstates;
    uint8_t voltage_domains;
    float frequency_mhz;
    uint32_t version_mask;
} asic_config_t;
#endif
