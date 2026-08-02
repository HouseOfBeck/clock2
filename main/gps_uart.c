#include "gps_uart.h"

#include <stdio.h>

#include "driver/uart.h"
#include "esp_check.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"

#include "nmea_timing.h"

#define GPS_UART         UART_NUM_1
#define GPS_RX_GPIO      45
#define GPS_TX_GPIO      UART_PIN_NO_CHANGE
#define GPS_BAUD_RATE    9600
#define GPS_BUFFER_SIZE  1024
#define GPS_UART_EVENT_QUEUE_SIZE 16
#define GPS_UART_RX_TIMEOUT_SYMBOLS 2

/* Set to 0 to stop printing raw NMEA without disabling timing analysis. */
#define GPS_UART_RAW_DUMP_ENABLED 0

/* Set to 1 to log event byte and completed-line counts. */
#define GPS_UART_EVENT_DIAGNOSTICS_ENABLED 0

static const char *TAG = "clock2-gps";
static QueueHandle_t gps_uart_event_queue;

esp_err_t gps_uart_start(void)
{
    const uart_config_t uart_config = {
        .baud_rate = GPS_BAUD_RATE,
        .data_bits = UART_DATA_8_BITS,
        .parity = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };

    ESP_RETURN_ON_ERROR(
        uart_driver_install(
            GPS_UART,
            GPS_BUFFER_SIZE * 2,
            0,
            GPS_UART_EVENT_QUEUE_SIZE,
            &gps_uart_event_queue,
            0),
        TAG,
        "UART driver installation failed");

    ESP_RETURN_ON_ERROR(
        uart_param_config(GPS_UART, &uart_config),
        TAG,
        "UART configuration failed");

    /*
     * Generate a UART_DATA event shortly after an idle gap. ESP-IDF defines
     * this threshold in UART symbol periods; two symbols are about 2.3 ms at
     * 9600 baud and avoid triggering on normal spacing between bytes.
     */
    ESP_RETURN_ON_ERROR(
        uart_set_rx_timeout(
            GPS_UART,
            GPS_UART_RX_TIMEOUT_SYMBOLS),
        TAG,
        "UART RX timeout configuration failed");

    /*
     * L76K with H1 and H2 moved to position B:
     *
     * L76K TX -> Pico GP5 -> ESP32-S3 GPIO45
     *
     * Receive-only is sufficient for this hardware test.
     */
    ESP_RETURN_ON_ERROR(
        uart_set_pin(
            GPS_UART,
            GPS_TX_GPIO,
            GPS_RX_GPIO,
            UART_PIN_NO_CHANGE,
            UART_PIN_NO_CHANGE),
        TAG,
        "UART pin configuration failed");

    ESP_LOGI(
        TAG,
        "Listening for L76K NMEA on GPIO%d at %d baud",
        GPS_RX_GPIO,
        GPS_BAUD_RATE);

    return ESP_OK;
}

void gps_uart_poll(void)
{
    uint8_t buffer[GPS_BUFFER_SIZE];
    uart_event_t event;

    if (xQueueReceive(
            gps_uart_event_queue,
            &event,
            pdMS_TO_TICKS(100)) != pdTRUE) {
        return;
    }

    if (event.type == UART_DATA) {
        /* Timestamp the event before reading or parsing any buffered data. */
        const int64_t event_timestamp_us = esp_timer_get_time();
        size_t remaining = event.size;
        size_t complete_lines = 0;

        while (remaining > 0) {
            const size_t requested =
                remaining < sizeof(buffer)
                    ? remaining
                    : sizeof(buffer);

            const int length = uart_read_bytes(
                GPS_UART,
                buffer,
                requested,
                portMAX_DELAY);

            if (length <= 0) {
                ESP_LOGW(
                    TAG,
                    "Could not read %zu bytes from UART data event",
                    remaining);
                break;
            }

            remaining -= (size_t)length;
            complete_lines += nmea_timing_process_uart_event_bytes(
                buffer,
                (size_t)length,
                event_timestamp_us,
                remaining);

#if GPS_UART_RAW_DUMP_ENABLED
            fwrite(buffer, 1, (size_t)length, stdout);
            fflush(stdout);
#endif
        }

#if GPS_UART_EVENT_DIAGNOSTICS_ENABLED
        ESP_LOGI(
            TAG,
            "UART event bytes=%zu complete_lines=%zu",
            event.size,
            complete_lines);
#endif
        return;
    }

    switch (event.type) {
    case UART_FIFO_OVF:
        ESP_LOGW(TAG, "UART hardware FIFO overflow; flushing input");
        uart_flush_input(GPS_UART);
        xQueueReset(gps_uart_event_queue);
        break;

    case UART_BUFFER_FULL:
        ESP_LOGW(TAG, "UART RX ring buffer full; flushing input");
        uart_flush_input(GPS_UART);
        xQueueReset(gps_uart_event_queue);
        break;

    case UART_BREAK:
        ESP_LOGW(TAG, "UART RX break");
        break;

    case UART_PARITY_ERR:
        ESP_LOGW(TAG, "UART parity error");
        break;

    case UART_FRAME_ERR:
        ESP_LOGW(TAG, "UART frame error");
        break;

    default:
        ESP_LOGW(TAG, "Unhandled UART event type %d", event.type);
        break;
    }
}
