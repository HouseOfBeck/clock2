#pragma once

#include <stdbool.h>

#include "esp_err.h"

esp_err_t oled_display_init(void);
bool oled_display_is_available(void);
