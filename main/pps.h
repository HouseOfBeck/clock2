#pragma once

#include <stdint.h>

#include "esp_err.h"

typedef struct {
    uint32_t count;
    int64_t timestamp_us;
} pps_snapshot_t;

esp_err_t pps_start(void);
void pps_get_snapshot(pps_snapshot_t *snapshot);
void pps_log_latest(void);
