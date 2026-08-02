#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct {
    int year;
    int month;
    int day;
    int hour;
    int minute;
    int second;
} timebase_utc_t;

typedef struct {
    bool valid;
    /* At pps_timestamp_us, UTC is exactly unix_seconds.000000. */
    int64_t unix_seconds;
    uint32_t pps_count;
    int64_t pps_timestamp_us;
    /* Monotonic age of that latest PPS edge when this snapshot was made. */
    int64_t age_us;
    char source[4];
} timebase_snapshot_t;

/* Small, deterministic helpers intended to be directly unit-testable. */
bool timebase_nmea_checksum_valid(const char *sentence);
bool timebase_parse_zda(const char *sentence, timebase_utc_t *utc);
bool timebase_parse_rmc(const char *sentence, timebase_utc_t *utc);
bool timebase_utc_to_unix(
    const timebase_utc_t *utc,
    int64_t *unix_seconds);
bool timebase_association_delay_valid(
    int64_t sentence_arrival_us,
    int64_t pps_timestamp_us,
    int64_t *delay_us);

void timebase_process_sentence(
    const char *sentence,
    int64_t estimated_arrival_us);
bool timebase_get_snapshot(timebase_snapshot_t *snapshot);

/* Rejected sentences preserve the anchor; freshness loss is handled here. */
void timebase_poll(void);
void timebase_log_demo(void);
