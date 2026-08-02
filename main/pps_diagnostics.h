#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#ifndef PPS_PATH_DIAGNOSTICS
#define PPS_PATH_DIAGNOSTICS 0
#endif

typedef struct {
    bool diagnostics_enabled;
    bool hardware_capture_available;
    const char *selected_edge;
    bool pulse_width_valid;
    int64_t pulse_width_us;
    uint64_t capture_samples;
    int64_t capture_delta_mean_us;
    int64_t capture_delta_minimum_us;
    int64_t capture_delta_maximum_us;
} pps_diagnostics_snapshot_t;

/* Start a parallel MCPWM hardware-capture path for the selected GPIO. */
esp_err_t pps_diagnostics_start(int gpio_num);

/* ISR-safe state capture only; this function never logs. */
void pps_diagnostics_record_edge_from_isr(
    int64_t isr_timestamp_us,
    int gpio_level,
    uint32_t isr_sequence,
    int64_t store_delay_us);

/* Read captured hardware state and emit diagnostics from task context. */
void pps_diagnostics_log_latest(void);
void pps_diagnostics_get_snapshot(pps_diagnostics_snapshot_t *snapshot);
