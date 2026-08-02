#pragma once

#include <stdbool.h>
#include <stdint.h>

#define NTP_UNIX_EPOCH_OFFSET 2208988800ULL

typedef struct {
    int64_t unix_seconds;
    int64_t microseconds;
    uint32_t ntp_seconds;
    uint32_t ntp_fraction;
} ntp_time_value_t;

/* Convert normalized Unix time to host-order NTP fields without floating point. */
bool ntp_time_from_unix(
    int64_t unix_seconds,
    int64_t microseconds,
    ntp_time_value_t *value);

/*
 * Project from a timebase snapshot to now. The snapshot invariant is:
 * At pps_timestamp_us, UTC is exactly unix_seconds.000000.
 */
bool ntp_time_from_pps(
    int64_t snapshot_unix_seconds,
    int64_t pps_timestamp_us,
    int64_t now_us,
    ntp_time_value_t *value);

/* Deterministic conversion/projection tests used only in diagnostic builds. */
bool ntp_time_run_self_tests(void);
