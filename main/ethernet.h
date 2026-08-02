#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

typedef struct {
    bool running;
    bool link_up;
    bool has_ipv4;
    uint32_t ipv4;
    uint32_t netmask;
    uint32_t gateway;
    uint8_t mac[6];
    int mosi_gpio;
    int miso_gpio;
    int sclk_gpio;
    int cs_gpio;
    int int_gpio;
    int reset_gpio;
    uint32_t spi_clock_hz;
} ethernet_snapshot_t;

esp_err_t ethernet_start(void);
bool ethernet_get_snapshot(ethernet_snapshot_t *snapshot);
