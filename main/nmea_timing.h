#pragma once

#include <stddef.h>
#include <stdint.h>

size_t nmea_timing_process_uart_event_bytes(
    const uint8_t *data,
    size_t length,
    int64_t event_timestamp_us,
    size_t later_event_bytes);
