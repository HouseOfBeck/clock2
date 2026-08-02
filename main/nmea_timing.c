#include "nmea_timing.h"

#include <inttypes.h>
#include <stdbool.h>
#include <string.h>

#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"

#include "pps.h"
#include "timebase.h"

#define NMEA_LINE_BUFFER_SIZE       128
#define NMEA_UTC_BUFFER_SIZE        24
#define NMEA_MAX_VALID_DELAY_US     1500000
#define NMEA_SUMMARY_SAMPLE_COUNT   60
#define NMEA_STATUS_STALE_US        10000000

/* 9600 baud, 8N1: one start bit, eight data bits, and one stop bit. */
#define NMEA_UART_BYTE_TIME_US       (1000000LL * 10 / 9600)

typedef enum {
    NMEA_SENTENCE_GGA,
    NMEA_SENTENCE_RMC,
    NMEA_SENTENCE_ZDA,
    NMEA_SENTENCE_COUNT,
    NMEA_SENTENCE_UNSELECTED,
} nmea_sentence_type_t;

typedef struct {
    uint64_t sample_count;
    int64_t delay_sum_us;
    int64_t minimum_delay_us;
    int64_t maximum_delay_us;
} nmea_timing_stats_t;

static const char *TAG = "clock2-nmea";

static char line_buffer[NMEA_LINE_BUFFER_SIZE];
static size_t line_length;
static bool discarding_line;
static bool line_awaiting_lf;
static int64_t cr_arrival_us;

static nmea_timing_stats_t timing_stats[NMEA_SENTENCE_COUNT];
static uint64_t total_valid_samples;
static portMUX_TYPE status_lock = portMUX_INITIALIZER_UNLOCKED;

typedef struct {
    bool fix_valid;
    bool position_valid;
    uint8_t satellites;
    int32_t hdop_milli;
    int32_t latitude_e7;
    int32_t longitude_e7;
    int32_t altitude_mm;
    int64_t last_valid_nmea_us;
} gps_state_t;

static gps_state_t gps_state = {
    .hdop_milli = -1,
};

static const char *sentence_type_name(nmea_sentence_type_t type)
{
    static const char *const names[NMEA_SENTENCE_COUNT] = {
        [NMEA_SENTENCE_GGA] = "GGA",
        [NMEA_SENTENCE_RMC] = "RMC",
        [NMEA_SENTENCE_ZDA] = "ZDA",
    };

    return names[type];
}

static nmea_sentence_type_t identify_sentence(
    const char *line,
    const char **utc_field)
{
    if (line[0] != '$') {
        return NMEA_SENTENCE_UNSELECTED;
    }

    const char *comma = strchr(line, ',');
    if (comma == NULL || comma - line < 4) {
        return NMEA_SENTENCE_UNSELECTED;
    }

    const char *type = comma - 3;
    nmea_sentence_type_t sentence_type;

    if (memcmp(type, "GGA", 3) == 0) {
        sentence_type = NMEA_SENTENCE_GGA;
    } else if (memcmp(type, "RMC", 3) == 0) {
        sentence_type = NMEA_SENTENCE_RMC;
    } else if (memcmp(type, "ZDA", 3) == 0) {
        sentence_type = NMEA_SENTENCE_ZDA;
    } else {
        return NMEA_SENTENCE_UNSELECTED;
    }

    *utc_field = comma + 1;
    return sentence_type;
}

static void extract_utc(const char *field, char *utc, size_t utc_size)
{
    size_t length = 0;

    while (field[length] != '\0' &&
           field[length] != ',' &&
           field[length] != '*' &&
           length < utc_size - 1) {
        utc[length] = field[length];
        length++;
    }

    if (length == 0) {
        strcpy(utc, "unknown");
    } else {
        utc[length] = '\0';
    }
}

static void print_summary(void)
{
    nmea_timing_stats_t stats_snapshot[NMEA_SENTENCE_COUNT];
    portENTER_CRITICAL(&status_lock);
    memcpy(stats_snapshot, timing_stats, sizeof(stats_snapshot));
    portEXIT_CRITICAL(&status_lock);

    ESP_LOGI(TAG, "NMEA estimated timing summary");

    for (nmea_sentence_type_t type = NMEA_SENTENCE_GGA;
         type < NMEA_SENTENCE_COUNT;
         type++) {
        const nmea_timing_stats_t *stats = &stats_snapshot[type];

        if (stats->sample_count == 0) {
            ESP_LOGI(TAG, "  %s: n=0", sentence_type_name(type));
            continue;
        }

        ESP_LOGI(
            TAG,
            "  %s: n=%" PRIu64 " mean=%" PRId64
            " us min=%" PRId64 " us max=%" PRId64 " us",
            sentence_type_name(type),
            stats->sample_count,
            stats->delay_sum_us / (int64_t)stats->sample_count,
            stats->minimum_delay_us,
            stats->maximum_delay_us);
    }
}

static void record_valid_sample(
    nmea_sentence_type_t type,
    int64_t delay_us)
{
    bool print_now;
    portENTER_CRITICAL(&status_lock);
    nmea_timing_stats_t *stats = &timing_stats[type];

    if (stats->sample_count == 0) {
        stats->minimum_delay_us = delay_us;
        stats->maximum_delay_us = delay_us;
    } else {
        if (delay_us < stats->minimum_delay_us) {
            stats->minimum_delay_us = delay_us;
        }
        if (delay_us > stats->maximum_delay_us) {
            stats->maximum_delay_us = delay_us;
        }
    }

    stats->sample_count++;
    stats->delay_sum_us += delay_us;
    total_valid_samples++;
    print_now = total_valid_samples % NMEA_SUMMARY_SAMPLE_COUNT == 0;
    portEXIT_CRITICAL(&status_lock);

    if (print_now) {
        print_summary();
    }
}

static bool copy_field(
    const char *sentence,
    size_t field_number,
    char *buffer,
    size_t buffer_size)
{
    const char *cursor = strchr(sentence, ',');
    if (cursor == NULL || field_number == 0 || buffer_size == 0) {
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

    size_t length = 0;
    while (cursor[length] != '\0' && cursor[length] != ',' &&
           cursor[length] != '*' && length < buffer_size - 1U) {
        buffer[length] = cursor[length];
        length++;
    }
    if ((cursor[length] != '\0' && cursor[length] != ',' &&
         cursor[length] != '*') || length == 0) {
        return false;
    }
    buffer[length] = '\0';
    return true;
}

static bool parse_unsigned_field(const char *text, uint32_t *value)
{
    if (text == NULL || *text == '\0') {
        return false;
    }
    uint32_t result = 0;
    for (const char *cursor = text; *cursor != '\0'; cursor++) {
        if (*cursor < '0' || *cursor > '9') {
            return false;
        }
        result = result * 10U + (uint32_t)(*cursor - '0');
    }
    *value = result;
    return true;
}

static bool parse_fixed_milli(const char *text, int32_t *value)
{
    if (text == NULL || *text == '\0') {
        return false;
    }

    bool negative = false;
    if (*text == '-' || *text == '+') {
        negative = *text == '-';
        text++;
    }
    if (*text == '\0') {
        return false;
    }

    int64_t whole = 0;
    bool have_digit = false;
    while (*text >= '0' && *text <= '9') {
        have_digit = true;
        whole = whole * 10 + (*text - '0');
        text++;
    }

    int32_t fraction = 0;
    int32_t scale = 100;
    if (*text == '.') {
        text++;
        while (*text >= '0' && *text <= '9') {
            if (scale > 0) {
                fraction += (*text - '0') * scale;
                scale /= 10;
            }
            text++;
        }
    }
    if (!have_digit || *text != '\0' || whole > INT32_MAX / 1000) {
        return false;
    }

    int64_t result = whole * 1000 + fraction;
    if (negative) {
        result = -result;
    }
    if (result < INT32_MIN || result > INT32_MAX) {
        return false;
    }
    *value = (int32_t)result;
    return true;
}

static bool parse_coordinate_e7(
    const char *text,
    size_t degree_digits,
    char hemisphere,
    int32_t *coordinate_e7)
{
    const size_t length = strlen(text);
    if (length < degree_digits + 2U) {
        return false;
    }

    uint32_t degrees = 0;
    for (size_t index = 0; index < degree_digits; index++) {
        if (text[index] < '0' || text[index] > '9') {
            return false;
        }
        degrees = degrees * 10U + (uint32_t)(text[index] - '0');
    }

    const char *minutes = text + degree_digits;
    if (minutes[0] < '0' || minutes[0] > '9' ||
        minutes[1] < '0' || minutes[1] > '9') {
        return false;
    }
    uint32_t minutes_scaled =
        (uint32_t)((minutes[0] - '0') * 10 + minutes[1] - '0') *
        1000000U;
    minutes += 2;
    if (*minutes == '.') {
        minutes++;
        uint32_t scale = 100000U;
        while (*minutes >= '0' && *minutes <= '9') {
            if (scale > 0U) {
                minutes_scaled += (uint32_t)(*minutes - '0') * scale;
                scale /= 10U;
            }
            minutes++;
        }
    }
    if (*minutes != '\0' || minutes_scaled >= 60000000U) {
        return false;
    }

    const uint32_t maximum_degrees = degree_digits == 2U ? 90U : 180U;
    if (degrees > maximum_degrees ||
        (degrees == maximum_degrees && minutes_scaled != 0U)) {
        return false;
    }

    int64_t result = (int64_t)degrees * 10000000LL +
        ((int64_t)minutes_scaled * 10000000LL + 30000000LL) /
            60000000LL;
    if (hemisphere == 'S' || hemisphere == 'W') {
        result = -result;
    } else if (hemisphere != 'N' && hemisphere != 'E') {
        return false;
    }
    if (result < INT32_MIN || result > INT32_MAX) {
        return false;
    }
    *coordinate_e7 = (int32_t)result;
    return true;
}

static void update_gps_status(
    const char *sentence,
    nmea_sentence_type_t type,
    int64_t arrival_us)
{
    if (!timebase_nmea_checksum_valid(sentence)) {
        return;
    }

    portENTER_CRITICAL(&status_lock);
    gps_state.last_valid_nmea_us = arrival_us;
    portEXIT_CRITICAL(&status_lock);

    if (type != NMEA_SENTENCE_GGA) {
        return;
    }

    char quality_text[4];
    char satellites_text[4];
    char hdop_text[16];
    char latitude_text[20];
    char ns_text[2];
    char longitude_text[20];
    char ew_text[2];
    char altitude_text[20];
    uint32_t quality;
    uint32_t satellites;

    if (!copy_field(sentence, 6, quality_text, sizeof(quality_text)) ||
        !copy_field(sentence, 7, satellites_text, sizeof(satellites_text)) ||
        !parse_unsigned_field(quality_text, &quality) ||
        !parse_unsigned_field(satellites_text, &satellites)) {
        return;
    }

    int32_t hdop_milli = -1;
    (void)(copy_field(sentence, 8, hdop_text, sizeof(hdop_text)) &&
           parse_fixed_milli(hdop_text, &hdop_milli));

    int32_t latitude_e7 = 0;
    int32_t longitude_e7 = 0;
    int32_t altitude_mm = 0;
    const bool position_valid = quality > 0U &&
        copy_field(sentence, 2, latitude_text, sizeof(latitude_text)) &&
        copy_field(sentence, 3, ns_text, sizeof(ns_text)) &&
        copy_field(sentence, 4, longitude_text, sizeof(longitude_text)) &&
        copy_field(sentence, 5, ew_text, sizeof(ew_text)) &&
        copy_field(sentence, 9, altitude_text, sizeof(altitude_text)) &&
        parse_coordinate_e7(
            latitude_text, 2, ns_text[0], &latitude_e7) &&
        parse_coordinate_e7(
            longitude_text, 3, ew_text[0], &longitude_e7) &&
        parse_fixed_milli(altitude_text, &altitude_mm);

    portENTER_CRITICAL(&status_lock);
    gps_state.fix_valid = quality > 0U;
    gps_state.position_valid = position_valid;
    gps_state.satellites = satellites > UINT8_MAX
                               ? UINT8_MAX
                               : (uint8_t)satellites;
    gps_state.hdop_milli = hdop_milli;
    if (position_valid) {
        gps_state.latitude_e7 = latitude_e7;
        gps_state.longitude_e7 = longitude_e7;
        gps_state.altitude_mm = altitude_mm;
    }
    portEXIT_CRITICAL(&status_lock);
}

static void process_line(int64_t estimated_arrival_us)
{
    line_buffer[line_length] = '\0';

    const char *utc_field = NULL;
    const nmea_sentence_type_t type =
        identify_sentence(line_buffer, &utc_field);

    if (type == NMEA_SENTENCE_UNSELECTED) {
        return;
    }

    update_gps_status(line_buffer, type, estimated_arrival_us);

    pps_snapshot_t pps;
    pps_get_snapshot(&pps);

    char utc[NMEA_UTC_BUFFER_SIZE];
    extract_utc(utc_field, utc, sizeof(utc));

    if (pps.count == 0) {
        ESP_LOGI(
            TAG,
            "NMEA %s utc=%s pps=none estimated_delay=unavailable",
            sentence_type_name(type),
            utc);
    } else {
        const int64_t delay_us =
            estimated_arrival_us - pps.timestamp_us;

        if (delay_us < 0 || delay_us > NMEA_MAX_VALID_DELAY_US) {
            ESP_LOGW(
                TAG,
                "NMEA %s utc=%s pps=%" PRIu32
                " estimated_delay=%" PRId64 " us invalid",
                sentence_type_name(type),
                utc,
                pps.count,
                delay_us);
        } else {
            ESP_LOGI(
                TAG,
                "NMEA %s utc=%s pps=%" PRIu32
                " estimated_delay=%" PRId64 " us",
                sentence_type_name(type),
                utc,
                pps.count,
                delay_us);

            record_valid_sample(type, delay_us);
        }
    }

    timebase_process_sentence(line_buffer, estimated_arrival_us);
}

static bool complete_line(int64_t estimated_arrival_us)
{
    if (discarding_line) {
        discarding_line = false;
        line_length = 0;
        return false;
    }

    if (line_length == 0) {
        return false;
    }

    process_line(estimated_arrival_us);
    line_length = 0;
    return true;
}

static bool process_byte(uint8_t byte, int64_t estimated_arrival_us)
{
    bool completed_line = false;

    if (line_awaiting_lf) {
        line_awaiting_lf = false;

        if (byte == '\n') {
            return complete_line(estimated_arrival_us);
        }

        /* A bare CR also terminates a line when no LF follows it. */
        completed_line = complete_line(cr_arrival_us);
    }

    if (byte == '\r') {
        if (discarding_line) {
            discarding_line = false;
            line_length = 0;
        } else if (line_length > 0) {
            /* Wait for a possible LF so CRLF uses the true final byte. */
            line_awaiting_lf = true;
            cr_arrival_us = estimated_arrival_us;
        }
        return completed_line;
    }

    if (byte == '\n') {
        return complete_line(estimated_arrival_us) || completed_line;
    }

    if (byte == '$') {
        line_length = 0;
        discarding_line = false;
    }

    if (discarding_line) {
        return completed_line;
    }

    if (byte < 0x20 || byte > 0x7e) {
        discarding_line = true;
        return completed_line;
    }

    if (line_length >= sizeof(line_buffer) - 1) {
        discarding_line = true;
        ESP_LOGW(TAG, "Discarding overlong NMEA line");
        return completed_line;
    }

    line_buffer[line_length++] = (char)byte;
    return completed_line;
}

size_t nmea_timing_process_uart_event_bytes(
    const uint8_t *data,
    size_t length,
    int64_t event_timestamp_us,
    size_t later_event_bytes)
{
    size_t complete_lines = 0;

    for (size_t index = 0; index < length; index++) {
        const size_t later_bytes =
            later_event_bytes + length - index - 1;

        /*
         * This is an estimate of when this byte finished arriving on the
         * wire. The event timestamp is captured when the task dequeues the
         * UART event, then shifted backward by the wire time of all later
         * bytes belonging to that same event.
         */
        const int64_t estimated_arrival_us =
            event_timestamp_us -
            (int64_t)later_bytes * NMEA_UART_BYTE_TIME_US;

        if (process_byte(data[index], estimated_arrival_us)) {
            complete_lines++;
        }
    }

    return complete_lines;
}

static nmea_sentence_timing_snapshot_t make_timing_snapshot(
    const nmea_timing_stats_t *stats)
{
    return (nmea_sentence_timing_snapshot_t) {
        .sample_count = stats->sample_count,
        .mean_delay_us = stats->sample_count == 0U
                             ? 0
                             : stats->delay_sum_us /
                                   (int64_t)stats->sample_count,
        .minimum_delay_us = stats->minimum_delay_us,
        .maximum_delay_us = stats->maximum_delay_us,
    };
}

void nmea_timing_get_snapshot(nmea_status_snapshot_t *snapshot)
{
    if (snapshot == NULL) {
        return;
    }

    gps_state_t gps;
    nmea_timing_stats_t stats[NMEA_SENTENCE_COUNT];
    portENTER_CRITICAL(&status_lock);
    gps = gps_state;
    memcpy(stats, timing_stats, sizeof(stats));
    portEXIT_CRITICAL(&status_lock);

    const int64_t now_us = esp_timer_get_time();
    const int64_t nmea_age_us = gps.last_valid_nmea_us == 0
                                    ? -1
                                    : now_us - gps.last_valid_nmea_us;
    const bool current = nmea_age_us >= 0 &&
                         nmea_age_us <= NMEA_STATUS_STALE_US;
    *snapshot = (nmea_status_snapshot_t) {
        .fix_valid = current && gps.fix_valid,
        .position_valid = current && gps.position_valid,
        .satellites = gps.satellites,
        .hdop_milli = gps.hdop_milli,
        .latitude_e7 = gps.latitude_e7,
        .longitude_e7 = gps.longitude_e7,
        .altitude_mm = gps.altitude_mm,
        .last_valid_nmea_age_us = nmea_age_us,
        .gga = make_timing_snapshot(&stats[NMEA_SENTENCE_GGA]),
        .rmc = make_timing_snapshot(&stats[NMEA_SENTENCE_RMC]),
        .zda = make_timing_snapshot(&stats[NMEA_SENTENCE_ZDA]),
    };
}
