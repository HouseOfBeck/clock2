#include "nmea_timing.h"

#include <inttypes.h>
#include <stdbool.h>
#include <string.h>

#include "esp_log.h"
#include "esp_timer.h"

#include "pps.h"

#define NMEA_LINE_BUFFER_SIZE       128
#define NMEA_UTC_BUFFER_SIZE        24
#define NMEA_MAX_VALID_DELAY_US     1500000
#define NMEA_SUMMARY_SAMPLE_COUNT   60

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

static nmea_timing_stats_t timing_stats[NMEA_SENTENCE_COUNT];
static uint64_t total_valid_samples;

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
    ESP_LOGI(TAG, "NMEA timing summary");

    for (nmea_sentence_type_t type = NMEA_SENTENCE_GGA;
         type < NMEA_SENTENCE_COUNT;
         type++) {
        const nmea_timing_stats_t *stats = &timing_stats[type];

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

    if (total_valid_samples % NMEA_SUMMARY_SAMPLE_COUNT == 0) {
        print_summary();
    }
}

static void process_line(void)
{
    line_buffer[line_length] = '\0';

    const char *utc_field = NULL;
    const nmea_sentence_type_t type =
        identify_sentence(line_buffer, &utc_field);

    if (type == NMEA_SENTENCE_UNSELECTED) {
        return;
    }

    const int64_t arrival_us = esp_timer_get_time();
    pps_snapshot_t pps;
    pps_get_snapshot(&pps);

    char utc[NMEA_UTC_BUFFER_SIZE];
    extract_utc(utc_field, utc, sizeof(utc));

    if (pps.count == 0) {
        ESP_LOGI(
            TAG,
            "NMEA %s utc=%s pps=none delay=unavailable",
            sentence_type_name(type),
            utc);
        return;
    }

    const int64_t delay_us = arrival_us - pps.timestamp_us;

    if (delay_us < 0 || delay_us > NMEA_MAX_VALID_DELAY_US) {
        ESP_LOGW(
            TAG,
            "NMEA %s utc=%s pps=%" PRIu32
            " delay=%" PRId64 " us invalid",
            sentence_type_name(type),
            utc,
            pps.count,
            delay_us);
        return;
    }

    ESP_LOGI(
        TAG,
        "NMEA %s utc=%s pps=%" PRIu32 " delay=%" PRId64 " us",
        sentence_type_name(type),
        utc,
        pps.count,
        delay_us);

    record_valid_sample(type, delay_us);
}

static void process_byte(uint8_t byte)
{
    if (byte == '\r' || byte == '\n') {
        if (discarding_line) {
            discarding_line = false;
            line_length = 0;
        } else if (line_length > 0) {
            process_line();
            line_length = 0;
        }
        return;
    }

    if (byte == '$') {
        line_length = 0;
        discarding_line = false;
    }

    if (discarding_line) {
        return;
    }

    if (byte < 0x20 || byte > 0x7e) {
        discarding_line = true;
        return;
    }

    if (line_length >= sizeof(line_buffer) - 1) {
        discarding_line = true;
        ESP_LOGW(TAG, "Discarding overlong NMEA line");
        return;
    }

    line_buffer[line_length++] = (char)byte;
}

void nmea_timing_process_bytes(const uint8_t *data, size_t length)
{
    for (size_t index = 0; index < length; index++) {
        process_byte(data[index]);
    }
}
