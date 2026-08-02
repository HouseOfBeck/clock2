#include "gps_uart.h"

#include <stdio.h>

#include "driver/uart.h"
#include "esp_check.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"

#include "nmea_timing.h"

#define GPS_UART         UART_NUM_1
#define GPS_RX_GPIO      45
#define GPS_TX_GPIO      UART_PIN_NO_CHANGE
#define GPS_BAUD_RATE    9600
#define GPS_BUFFER_SIZE  1024

/* Set to 0 to stop printing raw NMEA without disabling timing analysis. */
#define GPS_UART_RAW_DUMP_ENABLED 1

static const char *TAG = "clock2-gps";

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
            0,
            NULL,
            0),
        TAG,
        "UART driver installation failed");

    ESP_RETURN_ON_ERROR(
        uart_param_config(GPS_UART, &uart_config),
        TAG,
        "UART configuration failed");

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

    const int length = uart_read_bytes(
        GPS_UART,
        buffer,
        GPS_BUFFER_SIZE,
        pdMS_TO_TICKS(100));

    if (length > 0) {
        nmea_timing_process_bytes(buffer, length);

#if GPS_UART_RAW_DUMP_ENABLED
        fwrite(buffer, 1, length, stdout);
        fflush(stdout);
#endif
    }
}
