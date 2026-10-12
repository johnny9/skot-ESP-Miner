#ifndef ASIC_ESP_IDF_H
#define ASIC_ESP_IDF_H
#include <stdbool.h>
/* Arrays indexed by ASIC_IO_DATA / ASIC_IO_CONTROL. Configure before I/O. */
typedef struct { int port[2], tx_gpio[2], rx_gpio[2]; } asic_esp_idf_config_t;
#define ASIC_ESP_IDF_DEFAULT_CONFIG { .port = {1, 2}, .tx_gpio = {17, 43}, .rx_gpio = {18, 44} }
bool asic_esp_idf_configure(const asic_esp_idf_config_t *config);
#endif
