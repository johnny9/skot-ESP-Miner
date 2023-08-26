#ifndef COMMON_H_
#define COMMON_H_

typedef struct __attribute__((__packed__)) {
    uint8_t job_id;
    uint32_t nonce;
    uint32_t rolled_version;
} task_result;

#endif