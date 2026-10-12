#include "asic_context_internal.h"
#include <stdio.h>
#include "asic_esp_idf.h"
#include "serial.h"
#include "driver/uart.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_rom_sys.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static asic_esp_idf_config_t uart_pins = ASIC_ESP_IDF_DEFAULT_CONFIG;
static bool owned[2];
static uart_port_t port(asic_io_channel_t c) { return (uart_port_t)uart_pins.port[c]; }
static TickType_t ticks(uint32_t ms)
{
    TickType_t value = pdMS_TO_TICKS(ms);
    return ms && !value ? 1 : value;
}
static asic_err_t idf_open(void *u, asic_io_channel_t c, int baud)
{
    (void)u;
    if (uart_is_driver_installed(port(c)))
        return owned[c] ? uart_set_baudrate(port(c), baud) : ASIC_ERR_INVALID_STATE;
    owned[c] = false;
    uart_config_t config = {
        .baud_rate = baud, .data_bits = UART_DATA_8_BITS,
        .parity = UART_PARITY_DISABLE, .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE, .rx_flow_ctrl_thresh = 122,
    };
    esp_err_t err = uart_param_config(port(c), &config);
    if (err == ESP_OK) err = uart_set_pin(port(c), uart_pins.tx_gpio[c], uart_pins.rx_gpio[c],
                                         UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE);
    if (err == ESP_OK) err = uart_driver_install(port(c),
        c == ASIC_IO_DATA ? 2048 : 512,
        c == ASIC_IO_DATA ? 2048 : 512, 0, NULL, 0);
    if (err == ESP_OK) owned[c] = true;
    return err;
}
static void idf_close(void *u, asic_io_channel_t c)
{
    (void)u;
    if (owned[c] && uart_is_driver_installed(port(c))) (void)uart_driver_delete(port(c));
    owned[c] = false;
}
static bool idf_is_open(void *u, asic_io_channel_t c)
{ (void)u; return owned[c] && uart_is_driver_installed(port(c)); }
static int idf_read(void *u, asic_io_channel_t c, uint8_t *data, size_t size, uint32_t ms)
{ (void)u; return uart_read_bytes(port(c), data, size, ticks(ms)); }
static int idf_write(void *u, asic_io_channel_t c, const uint8_t *data, size_t size)
{ (void)u; return uart_write_bytes(port(c), data, size); }
static asic_err_t idf_baud(void *u, asic_io_channel_t c, int baud)
{ (void)u; return uart_set_baudrate(port(c), baud); }
static asic_err_t idf_flush(void *u, asic_io_channel_t c)
{ (void)u; return uart_flush_input(port(c)); }
static asic_err_t idf_drain(void *u, asic_io_channel_t c, uint32_t ms)
{ (void)u; return uart_wait_tx_done(port(c), ticks(ms)); }
static int64_t idf_time(void *u) { (void)u; return esp_timer_get_time(); }
static void idf_delay(void *u, uint32_t us)
{
    (void)u;
    TickType_t whole_ticks = pdMS_TO_TICKS(us / 1000);
    if (whole_ticks) vTaskDelay(whole_ticks);
    uint32_t remaining = us - whole_ticks * portTICK_PERIOD_MS * 1000;
    if (remaining) esp_rom_delay_us(remaining);
}
static void idf_log(void *u, asic_log_level_t level, const char *tag, const char *fmt, va_list args)
{
    (void)u;
    char message[384];
    vsnprintf(message, sizeof(message), fmt, args);
    /* Keep IDF's formatting, compile-time limits and runtime tag filtering. */
    switch (level) {
    case ASIC_LOG_ERROR: ESP_LOGE(tag, "%s", message); break;
    case ASIC_LOG_WARN: ESP_LOGW(tag, "%s", message); break;
    case ASIC_LOG_INFO: ESP_LOGI(tag, "%s", message); break;
    case ASIC_LOG_DEBUG: ESP_LOGD(tag, "%s", message); break;
    }

}
const asic_platform_ops_t *asic_platform_default(void)
{
    static const asic_platform_ops_t ops = {
        .open = idf_open, .close = idf_close, .is_open = idf_is_open,
        .read = idf_read, .write = idf_write, .set_baud = idf_baud,
        .flush_rx = idf_flush, .drain_tx = idf_drain,
        .time_us = idf_time, .delay_us = idf_delay, .log = idf_log,
    };
    return &ops;
}
bool asic_esp_idf_configure(const asic_esp_idf_config_t *config)
{
    if (!config || config->port[0] == config->port[1]) return false;
    for (int i = 0; i < 2; ++i) {
        if (config->port[i] < 0 || config->port[i] >= UART_NUM_MAX) return false;
    }
    if (!asic_platform_install(NULL, NULL)) return false;
    owned[0] = owned[1] = false;
    uart_pins = *config;
    return true;
}
