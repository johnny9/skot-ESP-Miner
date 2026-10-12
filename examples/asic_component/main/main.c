#include "asic.h"
#include <stdio.h>

void app_main(void)
{
    const asic_job_t job = {.version = 0x20000000, .ntime = 0x65010203, .nbits = 0x1d00ffff};
    uint8_t header[80];
    asic_job_header(&job, 0, job.version, header);
    printf("Built %zu-byte Bitcoin header without board initialization\n", sizeof(header));
}
