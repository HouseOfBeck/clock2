#include <inttypes.h>
#include <stdio.h>

#include "driver/gpio.h"
#include "driver/uart.h"
#include "esp_err.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "ethernet.h"

#define GPS_UART         UART_NUM_1
#define GPS_RX_GPIO      45
#define GPS_TX_GPIO      UART_PIN_NO_CHANGE
#define GPS_PPS_GPIO     43
#define GPS_BAUD_RATE    9600
#define GPS_BUFFER_SIZE  1024

static const char *TAG = "clock2-gps";

static portMUX_TYPE pps_lock = portMUX_INITIALIZER_UNLOCKED;

static volatile uint32_t pps_count;
static volatile int64_t pps_timestamp_us;
static volatile int64_t pps_previous_us;

static void IRAM_ATTR pps_isr_handler(void *arg)
{
    (void)arg;

    const int64_t now_us = esp_timer_get_time();

    portENTER_CRITICAL_ISR(&pps_lock);

    pps_previous_us = pps_timestamp_us;
    pps_timestamp_us = now_us;
    pps_count++;

    portEXIT_CRITICAL_ISR(&pps_lock);
}

static void initialize_pps(void)
{
    const gpio_config_t pps_config = {
        .pin_bit_mask = 1ULL << GPS_PPS_GPIO,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_POSEDGE,
    };

    ESP_ERROR_CHECK(gpio_config(&pps_config));
    ESP_ERROR_CHECK(gpio_install_isr_service(0));
    ESP_ERROR_CHECK(
        gpio_isr_handler_add(
            GPS_PPS_GPIO,
            pps_isr_handler,
            NULL));

    ESP_LOGI(
        TAG,
        "Listening for PPS rising edges on GPIO%d",
        GPS_PPS_GPIO);
}

static void initialize_gps_uart(void)
{
    const uart_config_t uart_config = {
        .baud_rate = GPS_BAUD_RATE,
        .data_bits = UART_DATA_8_BITS,
        .parity = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };

    ESP_ERROR_CHECK(
        uart_driver_install(
            GPS_UART,
            GPS_BUFFER_SIZE * 2,
            0,
            0,
            NULL,
            0));

    ESP_ERROR_CHECK(uart_param_config(GPS_UART, &uart_config));

    /*
     * L76K with H1 and H2 moved to position B:
     *
     * L76K TX -> Pico GP5 -> ESP32-S3 GPIO45
     *
     * Receive-only is sufficient for this hardware test.
     */
    ESP_ERROR_CHECK(
        uart_set_pin(
            GPS_UART,
            GPS_TX_GPIO,
            GPS_RX_GPIO,
            UART_PIN_NO_CHANGE,
            UART_PIN_NO_CHANGE));

    ESP_LOGI(
        TAG,
        "Listening for L76K NMEA on GPIO%d at %d baud",
        GPS_RX_GPIO,
        GPS_BAUD_RATE);
}

void app_main(void)
{
    initialize_gps_uart();
    initialize_pps();
    ESP_ERROR_CHECK(ethernet_start());

    ESP_LOGI(TAG, "Clock 2 GPS, PPS, and Ethernet hardware test");

    uint8_t buffer[GPS_BUFFER_SIZE + 1];
    uint32_t displayed_pps_count = 0;

    while (true) {
        const int length = uart_read_bytes(
            GPS_UART,
            buffer,
            GPS_BUFFER_SIZE,
            pdMS_TO_TICKS(100));

        if (length > 0) {
            buffer[length] = '\0';
            printf("%s", (char *)buffer);
            fflush(stdout);
        }

        uint32_t current_count;
        int64_t current_timestamp;
        int64_t previous_timestamp;

        portENTER_CRITICAL(&pps_lock);

        current_count = pps_count;
        current_timestamp = pps_timestamp_us;
        previous_timestamp = pps_previous_us;

        portEXIT_CRITICAL(&pps_lock);

        if (current_count != displayed_pps_count) {
            const int64_t interval_us =
                previous_timestamp == 0
                    ? 0
                    : current_timestamp - previous_timestamp;

            if (interval_us == 0) {
                ESP_LOGI(
                    TAG,
                    "PPS #%" PRIu32
                    " timestamp=%" PRId64 " us",
                    current_count,
                    current_timestamp);
            } else {
                ESP_LOGI(
                    TAG,
                    "PPS #%" PRIu32
                    " timestamp=%" PRId64
                    " us, interval=%" PRId64 " us",
                    current_count,
                    current_timestamp,
                    interval_us);
            }

            displayed_pps_count = current_count;
        }
    }
}
