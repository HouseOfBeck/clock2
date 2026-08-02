#include "pps_diagnostics.h"

#include <inttypes.h>
#include <limits.h>
#include <stdbool.h>
#include <stddef.h>

#include "driver/mcpwm_cap.h"
#include "esp_check.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"

#define PPS_CAPTURE_CALIBRATION_SAMPLES 16U
#define PPS_CAPTURE_SUMMARY_INTERVAL 60U
#define PPS_CAPTURE_MATCH_WINDOW_US 10000LL

typedef struct {
    int64_t rising_us;
    int64_t falling_us;
    int64_t store_delay_us;
    uint32_t rising_sequence;
    uint32_t falling_sequence;
    int rising_level;
    int falling_level;
} pps_edge_state_t;

typedef struct {
    uint64_t count;
    int64_t sum_us;
    int64_t minimum_us;
    int64_t maximum_us;
} pps_capture_stats_t;

static const char *TAG = "clock2-pps-diag";

#if PPS_PATH_DIAGNOSTICS
static portMUX_TYPE diagnostic_lock = portMUX_INITIALIZER_UNLOCKED;
static mcpwm_cap_timer_handle_t capture_timer;
static mcpwm_cap_channel_handle_t capture_channel;
static uint32_t capture_resolution_hz;
static int64_t capture_epoch_us;
static int64_t capture_calibration_uncertainty_us;
static bool capture_available;
static volatile bool capture_calibrating;
static volatile uint32_t previous_capture_value;
static volatile uint64_t latest_capture_ticks;
static volatile uint32_t latest_capture_sequence;
static volatile pps_edge_state_t edge_state;
static uint32_t logged_rising_sequence;
static uint32_t logged_falling_sequence;
static pps_capture_stats_t capture_stats;
#endif

#if PPS_PATH_DIAGNOSTICS
static int64_t capture_ticks_to_us(uint64_t ticks)
{
    const uint64_t whole_seconds = ticks / capture_resolution_hz;
    const uint64_t remainder = ticks % capture_resolution_hz;
    return (int64_t)(whole_seconds * 1000000ULL +
                     remainder * 1000000ULL / capture_resolution_hz);
}

/*
 * MCPWM latches the APB-clock counter in hardware at the GPIO edge. The
 * callback only unwraps consecutive 32-bit captures; its scheduling latency
 * does not enter the captured value. A bracketed software catch establishes
 * the one-time mapping into esp_timer microseconds and reports its bound.
 */
static bool IRAM_ATTR capture_event_callback(
    mcpwm_cap_channel_handle_t channel,
    const mcpwm_capture_event_data_t *event_data,
    void *user_data)
{
    (void)channel;
    (void)user_data;

    portENTER_CRITICAL_ISR(&diagnostic_lock);
    if (!capture_calibrating) {
        const uint32_t delta =
            (uint32_t)(event_data->cap_value - previous_capture_value);
        latest_capture_ticks += delta;
        previous_capture_value = event_data->cap_value;
        latest_capture_sequence++;
    }
    portEXIT_CRITICAL_ISR(&diagnostic_lock);
    return false;
}

static esp_err_t calibrate_capture_epoch(void)
{
    int64_t best_span_us = INT64_MAX;
    int64_t best_epoch_us = 0;
    uint32_t best_capture_value = 0;

    for (uint32_t sample = 0;
         sample < PPS_CAPTURE_CALIBRATION_SAMPLES;
         sample++) {
        const int64_t before_us = esp_timer_get_time();
        ESP_RETURN_ON_ERROR(
            mcpwm_capture_channel_trigger_soft_catch(capture_channel),
            TAG,
            "MCPWM calibration catch failed");
        uint32_t raw_count;
        ESP_RETURN_ON_ERROR(
            mcpwm_capture_get_latched_value(capture_channel, &raw_count),
            TAG,
            "MCPWM calibration read failed");
        const int64_t after_us = esp_timer_get_time();
        const int64_t span_us = after_us - before_us;

        if (span_us >= 0 && span_us < best_span_us) {
            const int64_t midpoint_us =
                before_us + (span_us + 1) / 2;
            best_span_us = span_us;
            best_epoch_us = midpoint_us;
            best_capture_value = raw_count;
        }
    }

    if (best_span_us == INT64_MAX) {
        return ESP_FAIL;
    }

    capture_epoch_us = best_epoch_us;
    capture_calibration_uncertainty_us = (best_span_us + 1) / 2;

    portENTER_CRITICAL(&diagnostic_lock);
    previous_capture_value = best_capture_value;
    latest_capture_ticks = 0;
    latest_capture_sequence = 0;
    portEXIT_CRITICAL(&diagnostic_lock);
    return ESP_OK;
}
#endif

esp_err_t pps_diagnostics_start(int gpio_num)
{
#if PPS_PATH_DIAGNOSTICS
    const mcpwm_capture_timer_config_t timer_config = {
        .group_id = 0,
        .clk_src = MCPWM_CAPTURE_CLK_SRC_DEFAULT,
        /* Ignored on S3; the actual APB resolution is queried below. */
        .resolution_hz = 0,
    };
    ESP_RETURN_ON_ERROR(
        mcpwm_new_capture_timer(&timer_config, &capture_timer),
        TAG,
        "Could not allocate PPS MCPWM capture timer");
    ESP_RETURN_ON_ERROR(
        mcpwm_capture_timer_get_resolution(
            capture_timer, &capture_resolution_hz),
        TAG,
        "Could not get PPS capture resolution");

    const mcpwm_capture_channel_config_t channel_config = {
        .gpio_num = gpio_num,
        .prescale = 1,
        .flags.pos_edge = true,
        .flags.neg_edge = false,
    };
    ESP_RETURN_ON_ERROR(
        mcpwm_new_capture_channel(
            capture_timer, &channel_config, &capture_channel),
        TAG,
        "Could not allocate PPS MCPWM capture channel");

    const mcpwm_capture_event_callbacks_t callbacks = {
        .on_cap = capture_event_callback,
    };
    ESP_RETURN_ON_ERROR(
        mcpwm_capture_channel_register_event_callbacks(
            capture_channel, &callbacks, NULL),
        TAG,
        "Could not register PPS capture callback");

    ESP_RETURN_ON_ERROR(
        mcpwm_capture_timer_enable(capture_timer),
        TAG,
        "Could not enable PPS MCPWM capture timer");
    capture_calibrating = true;
    ESP_RETURN_ON_ERROR(
        mcpwm_capture_channel_enable(capture_channel),
        TAG,
        "Could not enable PPS MCPWM capture channel");
    ESP_RETURN_ON_ERROR(
        mcpwm_capture_timer_start(capture_timer),
        TAG,
        "Could not start PPS MCPWM capture timer");

    ESP_RETURN_ON_ERROR(
        calibrate_capture_epoch(),
        TAG,
        "Could not calibrate PPS MCPWM capture timer");
    ESP_RETURN_ON_ERROR(
        mcpwm_capture_channel_disable(capture_channel),
        TAG,
        "Could not quiesce PPS capture after calibration");
    capture_calibrating = false;
    ESP_RETURN_ON_ERROR(
        mcpwm_capture_channel_enable(capture_channel),
        TAG,
        "Could not re-enable PPS MCPWM capture channel");

    capture_available = true;
    ESP_LOGI(
        TAG,
        "PPS MCPWM hardware capture enabled GPIO%d resolution=%" PRIu32
        " Hz calibration_uncertainty<=%" PRId64 " us",
        gpio_num,
        capture_resolution_hz,
        capture_calibration_uncertainty_us);
#else
    (void)gpio_num;
#endif
    return ESP_OK;
}

void pps_diagnostics_record_edge_from_isr(
    int64_t isr_timestamp_us,
    int gpio_level,
    uint32_t isr_sequence,
    int64_t store_delay_us)
{
#if PPS_PATH_DIAGNOSTICS
    portENTER_CRITICAL_ISR(&diagnostic_lock);
    if (gpio_level != 0) {
        edge_state.rising_us = isr_timestamp_us;
        edge_state.rising_sequence = isr_sequence;
        edge_state.rising_level = gpio_level;
        edge_state.store_delay_us = store_delay_us;
    } else {
        edge_state.falling_us = isr_timestamp_us;
        edge_state.falling_sequence = isr_sequence;
        edge_state.falling_level = gpio_level;
    }
    portEXIT_CRITICAL_ISR(&diagnostic_lock);
#else
    (void)isr_timestamp_us;
    (void)gpio_level;
    (void)isr_sequence;
    (void)store_delay_us;
#endif
}

void pps_diagnostics_log_latest(void)
{
#if PPS_PATH_DIAGNOSTICS
    pps_edge_state_t edges;
    portENTER_CRITICAL(&diagnostic_lock);
    edges = edge_state;
    portEXIT_CRITICAL(&diagnostic_lock);

    if (edges.rising_sequence != 0U &&
        edges.rising_sequence != logged_rising_sequence) {
        ESP_LOGI(
            TAG,
            "PPS edge seq=%" PRIu32 " level=%d isr_ts=%" PRId64
            " us stored_timestamp=%" PRId64
            " us store_delay=%" PRId64
            " us interrupt_status=unavailable selected_edge=rising",
            edges.rising_sequence,
            edges.rising_level,
            edges.rising_us,
            edges.rising_us,
            edges.store_delay_us);

        if (capture_available) {
            uint64_t captured_ticks;
            uint32_t capture_sequence;
            portENTER_CRITICAL(&diagnostic_lock);
            captured_ticks = latest_capture_ticks;
            capture_sequence = latest_capture_sequence;
            portEXIT_CRITICAL(&diagnostic_lock);

            if (capture_sequence != 0U) {
                const int64_t converted_us = capture_epoch_us +
                    capture_ticks_to_us(captured_ticks);
                const int64_t delta_us =
                    edges.rising_us - converted_us;
                const bool valid = delta_us >=
                                       -PPS_CAPTURE_MATCH_WINDOW_US &&
                                   delta_us <=
                                       PPS_CAPTURE_MATCH_WINDOW_US;

                ESP_LOGI(
                    TAG,
                    "PPS capture valid=%s isr_ts=%" PRId64
                    " us hw_ts=%" PRIu64 " ticks converted_us=%" PRId64
                    " us delta=%" PRId64
                    " us calibration_uncertainty<=%" PRId64 " us",
                    valid ? "yes" : "no",
                    edges.rising_us,
                    captured_ticks,
                    converted_us,
                    delta_us,
                    capture_calibration_uncertainty_us);

                if (valid) {
                    if (capture_stats.count == 0U ||
                        delta_us < capture_stats.minimum_us) {
                        capture_stats.minimum_us = delta_us;
                    }
                    if (capture_stats.count == 0U ||
                        delta_us > capture_stats.maximum_us) {
                        capture_stats.maximum_us = delta_us;
                    }
                    capture_stats.count++;
                    capture_stats.sum_us += delta_us;

                    if (capture_stats.count %
                            PPS_CAPTURE_SUMMARY_INTERVAL == 0U) {
                        ESP_LOGI(
                            TAG,
                            "PPS capture summary n=%" PRIu64
                            " delta_mean=%" PRId64
                            " us min=%" PRId64 " us max=%" PRId64 " us",
                            capture_stats.count,
                            capture_stats.sum_us /
                                (int64_t)capture_stats.count,
                            capture_stats.minimum_us,
                            capture_stats.maximum_us);
                    }
                }
            }
        }
        logged_rising_sequence = edges.rising_sequence;
    }

    if (edges.falling_sequence != 0U &&
        edges.falling_sequence != logged_falling_sequence &&
        edges.rising_us > 0 && edges.falling_us >= edges.rising_us) {
        ESP_LOGI(
            TAG,
            "PPS pulse rising=%" PRId64 " falling=%" PRId64
            " width=%" PRId64 " us selected_edge=rising",
            edges.rising_us,
            edges.falling_us,
            edges.falling_us - edges.rising_us);
        logged_falling_sequence = edges.falling_sequence;
    }
#endif
}
