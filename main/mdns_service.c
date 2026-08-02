#include "mdns_service.h"

#include <stdio.h>

#include "esp_log.h"
#include "mdns.h"

#include "app_config.h"

#define CLOCK2_INSTANCE_NAME "Clock 2 GPS NTP Server"
#define CLOCK2_HTTP_INSTANCE_NAME "Clock 2 Status"

static const char *TAG = "clock2-mdns";
static char active_hostname[APP_HOSTNAME_BUFFER_SIZE];
static char active_fqdn[APP_HOSTNAME_BUFFER_SIZE + sizeof(".local")];

esp_err_t clock2_mdns_start(void)
{
    esp_err_t err;
    app_config_snapshot_t config;
    app_config_get_snapshot(&config);
    snprintf(active_hostname, sizeof(active_hostname), "%s", config.active_hostname);
    snprintf(active_fqdn, sizeof(active_fqdn), "%s.local", config.active_hostname);

    err = mdns_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "mdns_init failed: %s", esp_err_to_name(err));
        return err;
    }

    err = mdns_hostname_set(active_hostname);
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

    mdns_txt_item_t http_txt[] = {
        {"path", "/"},
        {"product", "Clock2"},
    };
    err = mdns_service_add(
        CLOCK2_HTTP_INSTANCE_NAME,
        "_http",
        "_tcp",
        80,
        http_txt,
        sizeof(http_txt) / sizeof(http_txt[0]));
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "HTTP mDNS service failed: %s", esp_err_to_name(err));
        mdns_free();
        return err;
    }

    err = mdns_service_add(
        CLOCK2_INSTANCE_NAME,
        "_ntp",
        "_udp",
        123,
        NULL,
        0);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "NTP mDNS service failed: %s", esp_err_to_name(err));
        mdns_free();
        return err;
    }

    ESP_LOGI(TAG, "mDNS hostname: %s", active_fqdn);

    return ESP_OK;
}

const char *clock2_mdns_hostname(void)
{
    return active_fqdn;
}
