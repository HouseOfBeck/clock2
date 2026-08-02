#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

typedef struct {
    uint32_t count;
    int64_t timestamp_us;
} pps_snapshot_t;

typedef struct {
    bool valid;
    uint32_t count;
    int64_t timestamp_us;
    int64_t age_us;
    int64_t last_interval_us;
    uint64_t interval_samples;
    int64_t mean_interval_us;
    int64_t minimum_interval_us;
    int64_t maximum_interval_us;
} pps_status_snapshot_t;

esp_err_t pps_start(void);
void pps_get_snapshot(pps_snapshot_t *snapshot);
void pps_get_status_snapshot(pps_status_snapshot_t *snapshot);
int pps_get_gpio_num(void);
void pps_log_latest(void);
