#include <stdio.h>
#include "asic.h"

int main(void)
{
    asic_context_t asic = {0};
    const asic_config_t config = {
        .model = BM1370, .asic_count = 1, .core_count = 128,
        .small_core_count = 2040, .difficulty = 256,
        .default_timeout_ms = 500, .frequency_mhz = 525,
        .version_mask = BIP320_VERSION_ROLLING_MASK,
    };
    if (!asic_context_init(&asic, &config)) return 1;
    const asic_job_t job = {.version = 0x20000000, .ntime = 0x65010203, .nbits = 0x1d00ffff};
    uint8_t header[80];
    asic_job_header(&job, 0, job.version, header);
    printf("Built %zu-byte header; work interval %.0f ms\n", sizeof(header),
           ASIC_get_asic_job_frequency_ms(&asic));
    // Install transport callbacks and prepare board power/reset before ASIC_init.
    asic_context_destroy(&asic);
    return 0;
}
