#pragma once

#include <stdbool.h>

#include "esp_err.h"

typedef struct {
    int uart_num;
    int rx_gpio;
    int tx_gpio;
    int baud_rate;
    int rx_timeout_symbols;
    bool raw_dump_enabled;
    bool event_diagnostics_enabled;
} gps_uart_config_snapshot_t;

esp_err_t gps_uart_start(void);
void gps_uart_poll(void);
void gps_uart_get_config_snapshot(gps_uart_config_snapshot_t *snapshot);
