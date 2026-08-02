#pragma once

#include <stdint.h>

#include "esp_err.h"

#ifndef PPS_PATH_DIAGNOSTICS
#define PPS_PATH_DIAGNOSTICS 1
#endif

/* Start a parallel GPIO-ETM-to-GPTimer capture path for the selected GPIO. */
esp_err_t pps_diagnostics_start(int gpio_num);

/* ISR-safe state capture only; this function never logs. */
void pps_diagnostics_record_edge_from_isr(
    int64_t isr_timestamp_us,
    int gpio_level,
    uint32_t isr_sequence,
    int64_t store_delay_us);

/* Read captured hardware state and emit diagnostics from task context. */
void pps_diagnostics_log_latest(void);
