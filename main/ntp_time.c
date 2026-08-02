#include "ntp_time.h"

#include <limits.h>
#include <stddef.h>

#define MICROSECONDS_PER_SECOND 1000000LL

bool ntp_time_from_unix(
    int64_t unix_seconds,
    int64_t microseconds,
    ntp_time_value_t *value)
{
    if (value == NULL || microseconds < 0 ||
        microseconds >= MICROSECONDS_PER_SECOND || unix_seconds < 0) {
        return false;
    }

    value->unix_seconds = unix_seconds;
    value->microseconds = microseconds;
    value->ntp_seconds =
        (uint32_t)((uint64_t)unix_seconds + NTP_UNIX_EPOCH_OFFSET);
    value->ntp_fraction =
        (uint32_t)(((uint64_t)microseconds << 32) /
                   MICROSECONDS_PER_SECOND);
    return true;
}

bool ntp_time_from_pps(
    int64_t snapshot_unix_seconds,
    int64_t pps_timestamp_us,
    int64_t now_us,
    ntp_time_value_t *value)
{
    if (now_us < pps_timestamp_us) {
        return false;
    }

    const int64_t delta_us = now_us - pps_timestamp_us;
    const int64_t elapsed_seconds =
        delta_us / MICROSECONDS_PER_SECOND;
    if (snapshot_unix_seconds > INT64_MAX - elapsed_seconds) {
        return false;
    }

    return ntp_time_from_unix(
        snapshot_unix_seconds + elapsed_seconds,
        delta_us % MICROSECONDS_PER_SECOND,
        value);
}

static bool conversion_matches(
    int64_t microseconds,
    uint32_t expected_fraction)
{
    ntp_time_value_t value;
    return ntp_time_from_unix(0, microseconds, &value) &&
           value.ntp_seconds == (uint32_t)NTP_UNIX_EPOCH_OFFSET &&
           value.ntp_fraction == expected_fraction;
}

bool ntp_time_run_self_tests(void)
{
    ntp_time_value_t value;

    if (!conversion_matches(0, 0x00000000U) ||
        !conversion_matches(250000, 0x40000000U) ||
        !conversion_matches(500000, 0x80000000U) ||
        !conversion_matches(750000, 0xC0000000U)) {
        return false;
    }

    if (!ntp_time_from_unix(0, 999999, &value) ||
        value.ntp_seconds != (uint32_t)NTP_UNIX_EPOCH_OFFSET ||
        value.ntp_fraction >= UINT32_MAX) {
        return false;
    }

    /* More than one second after a PPS must normalize into the next second. */
    if (!ntp_time_from_pps(100, 1000000, 2250000, &value) ||
        value.unix_seconds != 101 || value.microseconds != 250000) {
        return false;
    }

    /*
     * Simulate a snapshot already advanced by one PPS count from an anchor:
     * anchor UTC 100/count 7 -> latest UTC 101/count 8, then add 500 ms.
     */
    const uint32_t anchor_count = 7;
    const uint32_t latest_count = 8;
    const int64_t latest_unix_seconds =
        100 + (int64_t)(uint32_t)(latest_count - anchor_count);
    if (!ntp_time_from_pps(
            latest_unix_seconds, 2000000, 2500000, &value) ||
        value.unix_seconds != 101 || value.microseconds != 500000) {
        return false;
    }

    return true;
}
