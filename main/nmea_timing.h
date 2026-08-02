#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct {
    uint64_t sample_count;
    int64_t mean_delay_us;
    int64_t minimum_delay_us;
    int64_t maximum_delay_us;
} nmea_sentence_timing_snapshot_t;

typedef struct {
    bool fix_valid;
    bool position_valid;
    uint8_t satellites;
    int32_t hdop_milli;
    int32_t latitude_e7;
    int32_t longitude_e7;
    int32_t altitude_mm;
    int64_t last_valid_nmea_age_us;
    nmea_sentence_timing_snapshot_t gga;
    nmea_sentence_timing_snapshot_t rmc;
    nmea_sentence_timing_snapshot_t zda;
} nmea_status_snapshot_t;

size_t nmea_timing_process_uart_event_bytes(
    const uint8_t *data,
    size_t length,
    int64_t event_timestamp_us,
    size_t later_event_bytes);

void nmea_timing_get_snapshot(nmea_status_snapshot_t *snapshot);
