#include "pps.h"

#include <inttypes.h>

#include "driver/gpio.h"
#include "esp_check.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"

#include "pps_diagnostics.h"

#define GPS_PPS_GPIO 43

static const char *TAG = "clock2-gps";

static portMUX_TYPE pps_lock = portMUX_INITIALIZER_UNLOCKED;

static volatile uint32_t pps_count;
static volatile int64_t pps_timestamp_us;
static volatile int64_t pps_previous_us;
static volatile uint32_t pps_isr_sequence;

static void IRAM_ATTR pps_isr_handler(void *arg)
{
    /* First executable operation: capture the software ISR timestamp. */
    const int64_t isr_timestamp_us = esp_timer_get_time();
    (void)arg;
    /*
     * ESP-IDF's per-pin service clears edge-triggered status before invoking
     * this handler, so no truthful interrupt-status value remains here.
     * The sampled level and ISR sequence are retained instead.
     */
    const int gpio_level = gpio_get_level(GPS_PPS_GPIO);
    int64_t store_delay_us = 0;
    uint32_t isr_sequence;

    portENTER_CRITICAL_ISR(&pps_lock);

    isr_sequence = ++pps_isr_sequence;
    if (gpio_level != 0) {
        /*
         * The measured write latency does not alter the captured value:
         * pps_timestamp_us remains the ISR-entry timestamp above.
         */
        store_delay_us = esp_timer_get_time() - isr_timestamp_us;
        pps_previous_us = pps_timestamp_us;
        pps_timestamp_us = isr_timestamp_us;
        pps_count++;
    }

    portEXIT_CRITICAL_ISR(&pps_lock);

    pps_diagnostics_record_edge_from_isr(
        isr_timestamp_us,
        gpio_level,
        isr_sequence,
        store_delay_us);
}

esp_err_t pps_start(void)
{
    const gpio_config_t pps_config = {
        .pin_bit_mask = 1ULL << GPS_PPS_GPIO,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
#if PPS_PATH_DIAGNOSTICS
        /* Falling edges are diagnostic only; rising remains authoritative. */
        .intr_type = GPIO_INTR_ANYEDGE,
#else
        .intr_type = GPIO_INTR_POSEDGE,
#endif
    };

    ESP_RETURN_ON_ERROR(
        gpio_config(&pps_config),
        TAG,
        "PPS GPIO configuration failed");

#if PPS_PATH_DIAGNOSTICS
    const esp_err_t diagnostic_result =
        pps_diagnostics_start(GPS_PPS_GPIO);
    if (diagnostic_result != ESP_OK) {
        ESP_LOGW(
            TAG,
            "PPS hardware capture unavailable: %s",
            esp_err_to_name(diagnostic_result));
    }
#endif

    ESP_RETURN_ON_ERROR(
        gpio_install_isr_service(0),
        TAG,
        "GPIO ISR service installation failed");

    ESP_RETURN_ON_ERROR(
        gpio_isr_handler_add(
            GPS_PPS_GPIO,
            pps_isr_handler,
            NULL),
        TAG,
        "PPS ISR registration failed");

    ESP_LOGI(
        TAG,
        "Listening for PPS rising edges on GPIO%d",
        GPS_PPS_GPIO);

    return ESP_OK;
}

void pps_get_snapshot(pps_snapshot_t *snapshot)
{
    portENTER_CRITICAL(&pps_lock);

    snapshot->count = pps_count;
    snapshot->timestamp_us = pps_timestamp_us;

    portEXIT_CRITICAL(&pps_lock);
}

void pps_log_latest(void)
{
    static uint32_t displayed_pps_count;

    uint32_t current_count;
    int64_t current_timestamp;
    int64_t previous_timestamp;

    portENTER_CRITICAL(&pps_lock);

    current_count = pps_count;
    current_timestamp = pps_timestamp_us;
    previous_timestamp = pps_previous_us;

    portEXIT_CRITICAL(&pps_lock);

    pps_diagnostics_log_latest();

    if (current_count == displayed_pps_count) {
        return;
    }

    const int64_t interval_us =
        previous_timestamp == 0
            ? 0
            : current_timestamp - previous_timestamp;

    if (interval_us == 0) {
        ESP_LOGI(
            TAG,
            "PPS #%" PRIu32 " timestamp=%" PRId64 " us",
            current_count,
            current_timestamp);
    } else {
        ESP_LOGI(
            TAG,
            "PPS #%" PRIu32 " timestamp=%" PRId64
            " us, interval=%" PRId64 " us",
            current_count,
            current_timestamp,
            interval_us);
    }

    displayed_pps_count = current_count;
}
