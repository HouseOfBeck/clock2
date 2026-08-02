#include "app_config.h"

#include <stdio.h>
#include <string.h>

#include "esp_check.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "nvs.h"
#include "nvs_flash.h"

#define APP_CONFIG_NVS_NAMESPACE "clock2"
#define APP_CONFIG_NVS_HOSTNAME_KEY "hostname"

static const char *TAG = "clock2-config";
static StaticSemaphore_t config_mutex_storage;
static SemaphoreHandle_t config_mutex;
static app_config_snapshot_t config_state;

static void copy_text(char *destination, size_t size, const char *source)
{
    if (destination != NULL && size > 0) {
        snprintf(destination, size, "%s", source);
    }
}

esp_err_t app_config_validate_hostname(
    const char *input,
    char *normalized,
    size_t normalized_size,
    char *error,
    size_t error_size)
{
    char candidate[APP_HOSTNAME_BUFFER_SIZE];
    const app_hostname_validation_result_t result =
        app_hostname_normalize(input, candidate);
    if (result != APP_HOSTNAME_VALID) {
        copy_text(error, error_size, app_hostname_validation_message(result));
        return ESP_ERR_INVALID_ARG;
    }
    if (normalized == NULL || normalized_size <= strlen(candidate)) {
        copy_text(error, error_size, "Hostname output buffer is too small.");
        return ESP_ERR_INVALID_SIZE;
    }
    copy_text(normalized, normalized_size, candidate);
    copy_text(error, error_size, "");
    return ESP_OK;
}

static esp_err_t initialize_nvs(void)
{
    esp_err_t result = nvs_flash_init();
    if (result == ESP_ERR_NVS_NO_FREE_PAGES ||
        result == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_LOGW(TAG, "Recovering NVS after initialization error: %s",
                 esp_err_to_name(result));
        ESP_RETURN_ON_ERROR(nvs_flash_erase(), TAG, "NVS erase failed");
        result = nvs_flash_init();
    }
    return result;
}

static esp_err_t load_hostname(char hostname[APP_HOSTNAME_BUFFER_SIZE])
{
    snprintf(hostname, APP_HOSTNAME_BUFFER_SIZE, "%s", APP_CONFIG_DEFAULT_HOSTNAME);

    nvs_handle_t handle;
    esp_err_t result = nvs_open(
        APP_CONFIG_NVS_NAMESPACE, NVS_READONLY, &handle);
    if (result == ESP_ERR_NVS_NOT_FOUND) {
        return ESP_OK;
    }
    if (result != ESP_OK) {
        return result;
    }

    char stored[APP_HOSTNAME_BUFFER_SIZE];
    size_t stored_size = sizeof(stored);
    result = nvs_get_str(handle, APP_CONFIG_NVS_HOSTNAME_KEY, stored, &stored_size);
    nvs_close(handle);
    if (result == ESP_ERR_NVS_NOT_FOUND) {
        return ESP_OK;
    }
    if (result == ESP_ERR_NVS_INVALID_LENGTH) {
        ESP_LOGW(TAG, "Stored hostname is too long; using default");
        return ESP_OK;
    }
    if (result != ESP_OK) {
        return result;
    }

    char normalized[APP_HOSTNAME_BUFFER_SIZE];
    const app_hostname_validation_result_t validation =
        app_hostname_normalize(stored, normalized);
    if (validation != APP_HOSTNAME_VALID) {
        ESP_LOGW(TAG, "Stored hostname is invalid; using default");
        return ESP_OK;
    }
    snprintf(hostname, APP_HOSTNAME_BUFFER_SIZE, "%s", normalized);
    return ESP_OK;
}

esp_err_t app_config_init(void)
{
    if (config_mutex != NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    config_mutex = xSemaphoreCreateMutexStatic(&config_mutex_storage);
    if (config_mutex == NULL) {
        return ESP_ERR_NO_MEM;
    }

    esp_err_t result = initialize_nvs();
    if (result != ESP_OK) {
        return result;
    }

    char hostname[APP_HOSTNAME_BUFFER_SIZE];
    result = load_hostname(hostname);
    if (result != ESP_OK) {
        return result;
    }

    xSemaphoreTake(config_mutex, portMAX_DELAY);
    snprintf(config_state.active_hostname, sizeof(config_state.active_hostname), "%s", hostname);
    snprintf(config_state.configured_hostname, sizeof(config_state.configured_hostname), "%s", hostname);
    config_state.hostname_restart_pending = false;
    xSemaphoreGive(config_mutex);

    ESP_LOGI(TAG, "Active hostname: %s.local", hostname);
    return ESP_OK;
}

void app_config_get_snapshot(app_config_snapshot_t *snapshot)
{
    if (snapshot == NULL) {
        return;
    }
    xSemaphoreTake(config_mutex, portMAX_DELAY);
    *snapshot = config_state;
    xSemaphoreGive(config_mutex);
}

esp_err_t app_config_save_hostname(const char *hostname)
{
    char normalized[APP_HOSTNAME_BUFFER_SIZE];
    esp_err_t result = app_config_validate_hostname(
        hostname, normalized, sizeof(normalized), NULL, 0);
    if (result != ESP_OK) {
        return result;
    }

    xSemaphoreTake(config_mutex, portMAX_DELAY);
    if (strcmp(normalized, config_state.configured_hostname) == 0) {
        xSemaphoreGive(config_mutex);
        return ESP_OK;
    }

    nvs_handle_t handle;
    result = nvs_open(APP_CONFIG_NVS_NAMESPACE, NVS_READWRITE, &handle);
    if (result == ESP_OK) {
        result = nvs_set_str(handle, APP_CONFIG_NVS_HOSTNAME_KEY, normalized);
        if (result == ESP_OK) {
            result = nvs_commit(handle);
        }
        nvs_close(handle);
    }
    if (result == ESP_OK) {
        snprintf(
            config_state.configured_hostname,
            sizeof(config_state.configured_hostname),
            "%s",
            normalized);
        config_state.hostname_restart_pending = app_hostname_values_differ(
            config_state.active_hostname, config_state.configured_hostname);
    }
    xSemaphoreGive(config_mutex);
    return result;
}
