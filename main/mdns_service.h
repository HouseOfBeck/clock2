#pragma once

#include "esp_err.h"

esp_err_t clock2_mdns_start(void);
const char *clock2_mdns_hostname(void);
