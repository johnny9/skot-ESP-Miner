#include "serial.h"
#include "asic_platform.h"
#include <limits.h>

asic_err_t SERIAL_init(void) { return asic_io_open(ASIC_IO_DATA, UART_FREQ); }
bool SERIAL_is_initialized(void) { return asic_io_is_open(ASIC_IO_DATA); }
asic_err_t SERIAL_wait_tx_done(uint32_t ms) { return asic_io_drain_tx(ASIC_IO_DATA, ms); }
asic_err_t SERIAL_set_baud(int baud)
{
    asic_err_t err = SERIAL_wait_tx_done(1000);
    return err == ASIC_OK ? asic_io_set_baud(ASIC_IO_DATA, baud) : err;
}
asic_err_t SERIAL_ensure_initialized(int baud)
{
    if (!SERIAL_is_initialized()) {
        asic_err_t err = SERIAL_init();
        if (err != ASIC_OK) return err;
    }
    return SERIAL_set_baud(baud);
}
asic_err_t SERIAL_prepare_session(int baud)
{
    asic_err_t err = SERIAL_ensure_initialized(baud);
    return err == ASIC_OK ? asic_io_flush_rx(ASIC_IO_DATA) : err;
}
int SERIAL_send(uint8_t *data, int len, bool debug)
{
    if (len <= 0) return -1;
    if (debug) ASIC_LOG_BUFFER_HEX("serial tx", data, len);
    return asic_io_write(ASIC_IO_DATA, data, (size_t)len);
}
int16_t SERIAL_rx(uint8_t *buf, uint16_t size, uint16_t timeout_ms)
{
    if (!buf || !size) return -1;
    if (size > INT16_MAX) size = INT16_MAX;
    size_t received = 0;
    int64_t deadline = asic_time_us() + (int64_t)timeout_ms * 1000;
    do {
        int64_t remaining = deadline - asic_time_us();
        uint32_t wait_ms = remaining > 0 ? (uint32_t)((remaining + 999) / 1000) : 0;
        int n = asic_io_read(ASIC_IO_DATA, buf + received, size - received, wait_ms);
        if (n < 0) return -1;
        if (!n) break;
        received += (size_t)n;
    } while (received < size && (!timeout_ms || asic_time_us() < deadline));
    return (int16_t)received;
}
void SERIAL_clear_buffer(void) { (void)asic_io_flush_rx(ASIC_IO_DATA); }
void SERIAL_debug_rx(void)
{
    uint8_t bytes[100];
    int n = SERIAL_rx(bytes, sizeof(bytes), 20);
    if (n > 0) ASIC_LOG_BUFFER_HEX("serial rx", bytes, n);
}
