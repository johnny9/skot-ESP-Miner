#ifndef SERIAL_H_
#define SERIAL_H_

#include "asic_platform.h"

typedef enum
{
    JOB_PACKET = 0,
    CMD_PACKET = 1,
} packet_type_t;

#define UART_FREQ 115200

int SERIAL_send(uint8_t *, int, bool);
asic_err_t SERIAL_init(void);
void SERIAL_debug_rx(void);
int16_t SERIAL_rx(uint8_t *, uint16_t, uint16_t);
void SERIAL_clear_buffer(void);
asic_err_t SERIAL_set_baud(int baud);
bool SERIAL_is_initialized(void);

asic_err_t SERIAL_wait_tx_done(uint32_t timeout_ms);
asic_err_t SERIAL_ensure_initialized(int baud);
asic_err_t SERIAL_prepare_session(int baud);

#endif /* SERIAL_H_ */
