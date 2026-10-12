#ifndef MINER_ASIC_H
#define MINER_ASIC_H
#include "esp_err.h"
#include <stdbool.h>
#include <stdint.h>
typedef struct GlobalState GlobalState;
typedef struct {
    uint64_t time_us;
    float hashrate;
} asic_domain_measurement_t;

bool miner_asic_context_init(GlobalState *state);
uint8_t miner_asic_init(GlobalState *state);
void miner_asic_set_frequency(GlobalState *state);
esp_err_t miner_asic_get_domain_measurement(GlobalState *state, uint8_t asic_nr, uint8_t domain_nr, asic_domain_measurement_t *measurement);
#endif
