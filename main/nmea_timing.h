#pragma once

#include <stddef.h>
#include <stdint.h>

void nmea_timing_process_bytes(const uint8_t *data, size_t length);
