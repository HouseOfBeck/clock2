#include "mdns_service.h"

#include "esp_log.h"
#include "mdns.h"

#define CLOCK2_MDNS_HOSTNAME "clock2"
#define CLOCK2_INSTANCE_NAME "Clock 2 GPS NTP Server"

static const char *TAG = "clock2-mdns";

esp_err_t clock2_mdns_start(void)
{
    esp_err_t err;

    err = mdns_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "mdns_init failed: %s", esp_err_to_name(err));
        return err;
    }

    err = mdns_hostname_set(CLOCK2_MDNS_HOSTNAME);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "mdns_hostname_set failed: %s",
                 esp_err_to_name(err));
        mdns_free();
        return err;
    }

    err = mdns_instance_name_set(CLOCK2_INSTANCE_NAME);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "mdns_instance_name_set failed: %s",
                 esp_err_to_name(err));
        mdns_free();
        return err;
    }

    ESP_LOGI(TAG, "mDNS hostname: %s.local",
             CLOCK2_MDNS_HOSTNAME);

    return ESP_OK;
}
