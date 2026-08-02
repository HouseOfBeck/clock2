#include <stdbool.h>

#include "esp_err.h"
#include "esp_log.h"

#include "ethernet.h"
#include "gps_uart.h"
#include "mdns_service.h"
#include "pps.h"

static const char *TAG = "clock2-gps";

void app_main(void)
{
    ESP_ERROR_CHECK(gps_uart_start());
    ESP_ERROR_CHECK(pps_start());
    ESP_ERROR_CHECK(ethernet_start());
    ESP_ERROR_CHECK(clock2_mdns_start());

    ESP_LOGI(TAG, "Clock 2 GPS, PPS, Ethernet, and mDNS hardware test");

    while (true) {
        gps_uart_poll();
        pps_log_latest();
    }
}
