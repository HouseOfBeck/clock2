#pragma once

#include "esp_err.h"

esp_err_t gps_uart_start(void);
void gps_uart_print_raw(void);
