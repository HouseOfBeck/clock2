#include "timebase.h"

#include <inttypes.h>
#include <stdio.h>
#include <string.h>

#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"

#include "pps.h"

#define TIMEBASE_ASSOCIATION_MIN_US       50000
#define TIMEBASE_ASSOCIATION_MAX_US       950000
#define TIMEBASE_PPS_TIMEOUT_US           2500000
#define TIMEBASE_GNSS_TIMEOUT_US          10000000
#define TIMEBASE_ZDA_PREFERENCE_US        2500000
#define TIMEBASE_HEALTH_ASSOCIATIONS      60
#define TIMEBASE_DEMO_INTERVAL_US         10000000

typedef enum {
    TIMEBASE_SOURCE_NONE,
    TIMEBASE_SOURCE_RMC,
    TIMEBASE_SOURCE_ZDA,
} timebase_source_t;

typedef struct {
    bool valid;
    int64_t anchor_unix_seconds;
    uint32_t anchor_pps_count;
    int64_t anchor_pps_timestamp_us;
    timebase_source_t source;
    int64_t last_update_us;
    int64_t last_zda_update_us;
    uint64_t accepted_associations;
    uint64_t rejected_associations;
} timebase_state_t;

static const char *TAG = "clock2-time";
static portMUX_TYPE timebase_lock = portMUX_INITIALIZER_UNLOCKED;
static timebase_state_t state;

static const char *source_name(timebase_source_t source)
{
    switch (source) {
    case TIMEBASE_SOURCE_ZDA:
        return "ZDA";
    case TIMEBASE_SOURCE_RMC:
        return "RMC";
    default:
        return "---";
    }
}

static int hex_value(char character)
{
    if (character >= '0' && character <= '9') {
        return character - '0';
    }
    if (character >= 'A' && character <= 'F') {
        return character - 'A' + 10;
    }
    if (character >= 'a' && character <= 'f') {
        return character - 'a' + 10;
    }
    return -1;
}

bool timebase_nmea_checksum_valid(const char *sentence)
{
    if (sentence == NULL || sentence[0] != '$') {
        return false;
    }

    uint8_t checksum = 0;
    const char *cursor = sentence + 1;

    while (*cursor != '\0' && *cursor != '*') {
        checksum ^= (uint8_t)*cursor;
        cursor++;
    }

    if (*cursor != '*' ||
        hex_value(cursor[1]) < 0 ||
        hex_value(cursor[2]) < 0 ||
        cursor[3] != '\0') {
        return false;
    }

    const uint8_t expected =
        (uint8_t)((hex_value(cursor[1]) << 4) |
                  hex_value(cursor[2]));

    return checksum == expected;
}

static bool sentence_is_type(const char *sentence, const char *type)
{
    if (sentence == NULL || sentence[0] != '$') {
        return false;
    }

    const char *comma = strchr(sentence, ',');
    return comma != NULL &&
           comma - sentence >= 4 &&
           memcmp(comma - 3, type, 3) == 0;
}

static bool get_field(
    const char *sentence,
    size_t field_number,
    const char **field,
    size_t *field_length)
{
    const char *cursor = strchr(sentence, ',');
    if (cursor == NULL || field_number == 0) {
        return false;
    }

    cursor++;

    for (size_t current = 1; current < field_number; current++) {
        cursor = strchr(cursor, ',');
        if (cursor == NULL) {
            return false;
        }
        cursor++;
    }

    const char *end = cursor;
    while (*end != '\0' && *end != ',' && *end != '*') {
        end++;
    }

    *field = cursor;
    *field_length = (size_t)(end - cursor);
    return true;
}

static bool parse_digits(
    const char *text,
    size_t length,
    int *value)
{
    if (length == 0) {
        return false;
    }

    int result = 0;
    for (size_t index = 0; index < length; index++) {
        if (text[index] < '0' || text[index] > '9') {
            return false;
        }
        result = result * 10 + text[index] - '0';
    }

    *value = result;
    return true;
}

static bool parse_utc_field(
    const char *field,
    size_t length,
    timebase_utc_t *utc)
{
    if (length < 6 ||
        !parse_digits(field, 2, &utc->hour) ||
        !parse_digits(field + 2, 2, &utc->minute) ||
        !parse_digits(field + 4, 2, &utc->second)) {
        return false;
    }

    if (length > 6) {
        if (field[6] != '.' || length == 7) {
            return false;
        }
        for (size_t index = 7; index < length; index++) {
            if (field[index] != '0') {
                return false;
            }
        }
    }

    return utc->hour <= 23 &&
           utc->minute <= 59 &&
           utc->second <= 59;
}

static bool parse_date_fields(
    const char *day_field,
    size_t day_length,
    const char *month_field,
    size_t month_length,
    const char *year_field,
    size_t year_length,
    timebase_utc_t *utc)
{
    return day_length == 2 &&
           month_length == 2 &&
           year_length == 4 &&
           parse_digits(day_field, day_length, &utc->day) &&
           parse_digits(month_field, month_length, &utc->month) &&
           parse_digits(year_field, year_length, &utc->year);
}

bool timebase_parse_zda(const char *sentence, timebase_utc_t *utc)
{
    const char *time_field;
    const char *day_field;
    const char *month_field;
    const char *year_field;
    size_t time_length;
    size_t day_length;
    size_t month_length;
    size_t year_length;

    if (utc == NULL || !sentence_is_type(sentence, "ZDA") ||
        !get_field(sentence, 1, &time_field, &time_length) ||
        !get_field(sentence, 2, &day_field, &day_length) ||
        !get_field(sentence, 3, &month_field, &month_length) ||
        !get_field(sentence, 4, &year_field, &year_length)) {
        return false;
    }

    memset(utc, 0, sizeof(*utc));
    return parse_utc_field(time_field, time_length, utc) &&
           parse_date_fields(
               day_field,
               day_length,
               month_field,
               month_length,
               year_field,
               year_length,
               utc);
}

bool timebase_parse_rmc(const char *sentence, timebase_utc_t *utc)
{
    const char *time_field;
    const char *status_field;
    const char *date_field;
    size_t time_length;
    size_t status_length;
    size_t date_length;

    if (utc == NULL || !sentence_is_type(sentence, "RMC") ||
        !get_field(sentence, 1, &time_field, &time_length) ||
        !get_field(sentence, 2, &status_field, &status_length) ||
        !get_field(sentence, 9, &date_field, &date_length) ||
        status_length != 1 || status_field[0] != 'A' ||
        date_length != 6) {
        return false;
    }

    memset(utc, 0, sizeof(*utc));

    int two_digit_year;
    if (!parse_utc_field(time_field, time_length, utc) ||
        !parse_digits(date_field, 2, &utc->day) ||
        !parse_digits(date_field + 2, 2, &utc->month) ||
        !parse_digits(date_field + 4, 2, &two_digit_year)) {
        return false;
    }

    utc->year = two_digit_year >= 80
                    ? 1900 + two_digit_year
                    : 2000 + two_digit_year;
    return true;
}

static bool is_leap_year(int year)
{
    return year % 4 == 0 &&
           (year % 100 != 0 || year % 400 == 0);
}

static int days_in_month(int year, int month)
{
    static const uint8_t days[] = {
        31, 28, 31, 30, 31, 30,
        31, 31, 30, 31, 30, 31,
    };

    if (month == 2 && is_leap_year(year)) {
        return 29;
    }
    return days[month - 1];
}

bool timebase_utc_to_unix(
    const timebase_utc_t *utc,
    int64_t *unix_seconds)
{
    if (utc == NULL || unix_seconds == NULL ||
        utc->year < 1970 || utc->year > 9999 ||
        utc->month < 1 || utc->month > 12 ||
        utc->day < 1 ||
        utc->day > days_in_month(utc->year, utc->month) ||
        utc->hour < 0 || utc->hour > 23 ||
        utc->minute < 0 || utc->minute > 59 ||
        utc->second < 0 || utc->second > 59) {
        return false;
    }

    int year = utc->year;
    const int month = utc->month;
    year -= month <= 2;
    const int era = year / 400;
    const unsigned year_of_era = (unsigned)(year - era * 400);
    const unsigned day_of_year =
        (153U * (unsigned)(month + (month > 2 ? -3 : 9)) + 2U) /
            5U +
        (unsigned)utc->day - 1U;
    const unsigned day_of_era =
        year_of_era * 365U +
        year_of_era / 4U -
        year_of_era / 100U +
        day_of_year;
    const int64_t days_since_epoch =
        (int64_t)era * 146097 + day_of_era - 719468;

    *unix_seconds =
        days_since_epoch * 86400 +
        utc->hour * 3600 +
        utc->minute * 60 +
        utc->second;
    return true;
}

bool timebase_association_delay_valid(
    int64_t sentence_arrival_us,
    int64_t pps_timestamp_us,
    int64_t *delay_us)
{
    const int64_t delay = sentence_arrival_us - pps_timestamp_us;
    if (delay_us != NULL) {
        *delay_us = delay;
    }
    return delay >= TIMEBASE_ASSOCIATION_MIN_US &&
           delay <= TIMEBASE_ASSOCIATION_MAX_US;
}

static void unix_to_utc(int64_t unix_seconds, timebase_utc_t *utc)
{
    int64_t days = unix_seconds / 86400;
    int64_t seconds_of_day = unix_seconds % 86400;
    int64_t adjusted_days = days + 719468;
    const int64_t era = adjusted_days / 146097;
    const unsigned day_of_era =
        (unsigned)(adjusted_days - era * 146097);
    const unsigned year_of_era =
        (day_of_era - day_of_era / 1460U +
         day_of_era / 36524U - day_of_era / 146096U) /
        365U;
    int year = (int)year_of_era + (int)era * 400;
    const unsigned day_of_year =
        day_of_era -
        (365U * year_of_era + year_of_era / 4U -
         year_of_era / 100U);
    const unsigned month_prime = (5U * day_of_year + 2U) / 153U;

    utc->day = (int)(day_of_year -
                     (153U * month_prime + 2U) / 5U + 1U);
    utc->month =
        (int)month_prime + (month_prime < 10U ? 3 : -9);
    year += utc->month <= 2;
    utc->year = year;
    utc->hour = (int)(seconds_of_day / 3600);
    utc->minute = (int)((seconds_of_day % 3600) / 60);
    utc->second = (int)(seconds_of_day % 60);
}

static void format_utc(
    int64_t unix_seconds,
    char *buffer,
    size_t buffer_size)
{
    timebase_utc_t utc;
    unix_to_utc(unix_seconds, &utc);
    snprintf(
        buffer,
        buffer_size,
        "%04d-%02d-%02dT%02d:%02d:%02dZ",
        utc.year,
        utc.month,
        utc.day,
        utc.hour,
        utc.minute,
        utc.second);
}

bool timebase_format_unix_utc(
    int64_t unix_seconds,
    char *buffer,
    size_t buffer_size)
{
    if (buffer == NULL || buffer_size < 21U || unix_seconds < 0) {
        return false;
    }
    format_utc(unix_seconds, buffer, buffer_size);
    return true;
}

static void reject_association(
    const char *reason,
    bool include_delay,
    int64_t delay_us)
{
    /*
     * Sentence rejection is non-destructive: it records the bad candidate
     * but deliberately leaves validity, the UTC/PPS anchor, source
     * preference, and GNSS freshness timestamp unchanged.
     */
    portENTER_CRITICAL(&timebase_lock);
    state.rejected_associations++;
    portEXIT_CRITICAL(&timebase_lock);

    if (include_delay) {
        ESP_LOGW(
            TAG,
            "Timebase rejected: reason=%s delay=%" PRId64 " us",
            reason,
            delay_us);
    } else {
        ESP_LOGW(TAG, "Timebase rejected: reason=%s", reason);
    }
}

bool timebase_get_snapshot(timebase_snapshot_t *snapshot)
{
    if (snapshot == NULL) {
        return false;
    }

    pps_snapshot_t pps;
    pps_get_snapshot(&pps);
    const int64_t now_us = esp_timer_get_time();

    timebase_state_t current;
    portENTER_CRITICAL(&timebase_lock);
    current = state;
    portEXIT_CRITICAL(&timebase_lock);

    memset(snapshot, 0, sizeof(*snapshot));

    const int64_t pps_age_us = now_us - pps.timestamp_us;
    const int64_t gnss_age_us = now_us - current.last_update_us;
    const uint32_t pps_delta =
        (uint32_t)(pps.count - current.anchor_pps_count);

    if (!current.valid || pps.timestamp_us == 0 ||
        pps_age_us < 0 || pps_age_us > TIMEBASE_PPS_TIMEOUT_US ||
        gnss_age_us < 0 || gnss_age_us > TIMEBASE_GNSS_TIMEOUT_US ||
        pps_delta > UINT32_MAX / 2U) {
        return false;
    }

    snapshot->valid = true;
    /*
     * The anchor is advanced by the unsigned PPS-count delta, while the
     * timestamp comes from that same latest PPS snapshot. Therefore:
     * At pps_timestamp_us, UTC is exactly unix_seconds.000000.
     */
    snapshot->unix_seconds =
        current.anchor_unix_seconds + (int64_t)pps_delta;
    snapshot->pps_count = pps.count;
    snapshot->pps_timestamp_us = pps.timestamp_us;
    snapshot->age_us = pps_age_us;
    memcpy(snapshot->source, source_name(current.source), 4);
    return true;
}

void timebase_get_status_snapshot(timebase_status_snapshot_t *snapshot)
{
    if (snapshot == NULL) {
        return;
    }

    pps_snapshot_t pps;
    pps_get_snapshot(&pps);
    const int64_t now_us = esp_timer_get_time();

    timebase_state_t current;
    portENTER_CRITICAL(&timebase_lock);
    current = state;
    portEXIT_CRITICAL(&timebase_lock);

    memset(snapshot, 0, sizeof(*snapshot));
    snapshot->pps_count = pps.count;
    snapshot->pps_age_us = pps.timestamp_us == 0
                               ? -1
                               : now_us - pps.timestamp_us;
    snapshot->gnss_label_age_us = current.last_update_us == 0
                                      ? -1
                                      : now_us - current.last_update_us;
    snapshot->accepted_associations = current.accepted_associations;
    snapshot->rejected_associations = current.rejected_associations;
    memcpy(snapshot->source, source_name(current.source), 4);

    const uint32_t pps_delta =
        (uint32_t)(pps.count - current.anchor_pps_count);
    snapshot->valid = current.valid && pps.timestamp_us != 0 &&
                      snapshot->pps_age_us >= 0 &&
                      snapshot->pps_age_us <= TIMEBASE_PPS_TIMEOUT_US &&
                      snapshot->gnss_label_age_us >= 0 &&
                      snapshot->gnss_label_age_us <=
                          TIMEBASE_GNSS_TIMEOUT_US &&
                      pps_delta <= UINT32_MAX / 2U;
    if (snapshot->valid) {
        snapshot->unix_seconds =
            current.anchor_unix_seconds + (int64_t)pps_delta;
    }
}

void timebase_get_config_snapshot(timebase_config_snapshot_t *snapshot)
{
    if (snapshot == NULL) {
        return;
    }

    *snapshot = (timebase_config_snapshot_t) {
        .association_min_us = TIMEBASE_ASSOCIATION_MIN_US,
        .association_max_us = TIMEBASE_ASSOCIATION_MAX_US,
        .pps_timeout_us = TIMEBASE_PPS_TIMEOUT_US,
        .gnss_timeout_us = TIMEBASE_GNSS_TIMEOUT_US,
    };
}

static void log_health_summary(void)
{
    timebase_snapshot_t snapshot;
    const bool valid = timebase_get_snapshot(&snapshot);
    const int64_t now_us = esp_timer_get_time();

    timebase_state_t current;
    portENTER_CRITICAL(&timebase_lock);
    current = state;
    portEXIT_CRITICAL(&timebase_lock);

    char utc_text[24] = "unavailable";
    if (valid) {
        format_utc(snapshot.unix_seconds, utc_text, sizeof(utc_text));
    }

    ESP_LOGI(
        TAG,
        "Timebase health: valid=%s utc=%s pps=%" PRIu32
        " pps_age=%" PRId64 " us gnss_age=%" PRId64
        " us source=%s accepted=%" PRIu64 " rejected=%" PRIu64,
        valid ? "true" : "false",
        utc_text,
        valid ? snapshot.pps_count : 0,
        valid ? snapshot.age_us : -1,
        current.last_update_us == 0
            ? -1
            : now_us - current.last_update_us,
        source_name(current.source),
        current.accepted_associations,
        current.rejected_associations);
}

void timebase_process_sentence(
    const char *sentence,
    int64_t estimated_arrival_us)
{
    timebase_source_t candidate_source;
    timebase_utc_t utc;

    if (sentence_is_type(sentence, "ZDA")) {
        candidate_source = TIMEBASE_SOURCE_ZDA;
    } else if (sentence_is_type(sentence, "RMC")) {
        candidate_source = TIMEBASE_SOURCE_RMC;
    } else {
        return;
    }

    if (!timebase_nmea_checksum_valid(sentence)) {
        reject_association("checksum", false, 0);
        return;
    }

    const bool parsed =
        candidate_source == TIMEBASE_SOURCE_ZDA
            ? timebase_parse_zda(sentence, &utc)
            : timebase_parse_rmc(sentence, &utc);

    int64_t candidate_unix_seconds;
    if (!parsed || !timebase_utc_to_unix(&utc, &candidate_unix_seconds)) {
        reject_association(
            candidate_source == TIMEBASE_SOURCE_ZDA
                ? "invalid-zda"
                : "invalid-rmc",
            false,
            0);
        return;
    }

    pps_snapshot_t pps;
    pps_get_snapshot(&pps);
    if (pps.timestamp_us == 0) {
        reject_association("no-pps", false, 0);
        return;
    }

    int64_t association_delay_us;
    /*
     * The UART-derived timestamp is only a plausibility gate. It is never
     * used as the UTC boundary; the actual PPS ISR snapshot below is.
     */
    if (!timebase_association_delay_valid(
            estimated_arrival_us,
            pps.timestamp_us,
            &association_delay_us)) {
        reject_association(
            "association-window",
            true,
            association_delay_us);
        return;
    }

    const int64_t now_us = esp_timer_get_time();
    bool synchronized = false;
    bool source_changed = false;
    bool rejected_backward = false;
    bool rejected_projection = false;
    int64_t projection_difference = 0;
    uint64_t accepted_count = 0;

    portENTER_CRITICAL(&timebase_lock);

    if (state.valid) {
        const uint32_t pps_delta =
            (uint32_t)(pps.count - state.anchor_pps_count);

        if (pps_delta > UINT32_MAX / 2U) {
            rejected_backward = true;
        } else {
            const int64_t projected_unix_seconds =
                state.anchor_unix_seconds + (int64_t)pps_delta;
            projection_difference =
                candidate_unix_seconds - projected_unix_seconds;

            if (candidate_unix_seconds < projected_unix_seconds) {
                rejected_backward = true;
            } else if (projection_difference > 1) {
                rejected_projection = true;
            }
        }
    } else if (state.source != TIMEBASE_SOURCE_NONE &&
               candidate_unix_seconds < state.anchor_unix_seconds) {
        rejected_backward = true;
    } else {
        synchronized = true;
    }

    if (!rejected_backward && !rejected_projection) {
        const timebase_source_t previous_source = state.source;
        const bool keep_recent_zda_anchor =
            state.valid &&
            state.source == TIMEBASE_SOURCE_ZDA &&
            candidate_source == TIMEBASE_SOURCE_RMC &&
            now_us - state.last_zda_update_us <=
                TIMEBASE_ZDA_PREFERENCE_US;

        if (!keep_recent_zda_anchor) {
            /*
             * L76K RMC/ZDA labels the most recent PPS. At the stored
             * pps_timestamp_us, anchor_unix_seconds is exactly .000000.
             */
            state.anchor_unix_seconds = candidate_unix_seconds;
            state.anchor_pps_count = pps.count;
            state.anchor_pps_timestamp_us = pps.timestamp_us;
            state.source = candidate_source;
        }

        if (candidate_source == TIMEBASE_SOURCE_ZDA) {
            state.last_zda_update_us = now_us;
        }

        state.valid = true;
        state.last_update_us = now_us;
        state.accepted_associations++;
        accepted_count = state.accepted_associations;
        source_changed =
            previous_source != TIMEBASE_SOURCE_NONE &&
            state.source != previous_source;
    } else {
        /* Preserve the current anchor and freshness on isolated bad labels. */
        state.rejected_associations++;
    }

    portEXIT_CRITICAL(&timebase_lock);

    if (rejected_backward) {
        ESP_LOGW(TAG, "Timebase rejected: reason=utc-backward");
        return;
    }
    if (rejected_projection) {
        ESP_LOGW(
            TAG,
            "Timebase rejected: reason=projection difference=%" PRId64
            " s",
            projection_difference);
        return;
    }

    char utc_text[24];
    format_utc(candidate_unix_seconds, utc_text, sizeof(utc_text));

    if (synchronized) {
        ESP_LOGI(
            TAG,
            "Timebase synchronized: %s pps=%" PRIu32 " source=%s",
            utc_text,
            pps.count,
            source_name(candidate_source));
    } else if (source_changed) {
        ESP_LOGI(
            TAG,
            "Timebase confirmed: pps=%" PRIu32 " utc=%s source=%s",
            pps.count,
            utc_text,
            source_name(candidate_source));
    }

    if (accepted_count % TIMEBASE_HEALTH_ASSOCIATIONS == 0) {
        log_health_summary();
    }
}

void timebase_poll(void)
{
    pps_snapshot_t pps;
    pps_get_snapshot(&pps);
    const int64_t now_us = esp_timer_get_time();

    const char *loss_reason = NULL;

    portENTER_CRITICAL(&timebase_lock);

    /*
     * Freshness timeout, unlike sentence rejection, invalidates an existing
     * timebase. Guarding this with state.valid also makes loss logging a
     * single valid-to-invalid transition.
     */
    if (state.valid) {
        if (pps.timestamp_us == 0 ||
            now_us - pps.timestamp_us > TIMEBASE_PPS_TIMEOUT_US) {
            state.valid = false;
            loss_reason = "pps-timeout";
        } else if (now_us - state.last_update_us >
                   TIMEBASE_GNSS_TIMEOUT_US) {
            state.valid = false;
            loss_reason = "gnss-timeout";
        }
    }

    portEXIT_CRITICAL(&timebase_lock);

    if (loss_reason != NULL) {
        ESP_LOGW(TAG, "Timebase lost: reason=%s", loss_reason);
    }
}

void timebase_log_demo(void)
{
    static int64_t last_log_us;

    const int64_t now_us = esp_timer_get_time();
    if (last_log_us != 0 &&
        now_us - last_log_us < TIMEBASE_DEMO_INTERVAL_US) {
        return;
    }

    timebase_snapshot_t snapshot;
    if (!timebase_get_snapshot(&snapshot)) {
        return;
    }

    char utc_text[24];
    format_utc(snapshot.unix_seconds, utc_text, sizeof(utc_text));

    ESP_LOGI(
        TAG,
        "UTC timebase: %s pps=%" PRIu32 " age=%" PRId64
        " us source=%s",
        utc_text,
        snapshot.pps_count,
        snapshot.age_us,
        snapshot.source);

    last_log_us = now_us;
}
