#pragma once

#include "esp_err.h"

esp_err_t pps_start(void);
void pps_log_latest(void);
