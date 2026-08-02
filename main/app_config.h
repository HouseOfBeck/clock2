#pragma once

#include <stdbool.h>
#include <stddef.h>

#include "esp_err.h"
#include "hostname_validation.h"

#define APP_CONFIG_DEFAULT_HOSTNAME "clock2"

typedef struct {
    char active_hostname[APP_HOSTNAME_BUFFER_SIZE];
    char configured_hostname[APP_HOSTNAME_BUFFER_SIZE];
    bool hostname_restart_pending;
} app_config_snapshot_t;

esp_err_t app_config_init(void);

void app_config_get_snapshot(app_config_snapshot_t *snapshot);

esp_err_t app_config_validate_hostname(
    const char *input,
    char *normalized,
    size_t normalized_size,
    char *error,
    size_t error_size);

esp_err_t app_config_save_hostname(const char *hostname);
