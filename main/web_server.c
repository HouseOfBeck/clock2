#include "web_server.h"

#include <inttypes.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "esp_app_desc.h"
#include "esp_chip_info.h"
#include "esp_check.h"
#include "esp_flash.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "esp_idf_version.h"
#include "lwip/ip4_addr.h"

#include "ethernet.h"
#include "gps_uart.h"
#include "mdns_service.h"
#include "nmea_timing.h"
#include "ntp_path_diagnostics.h"
#include "ntp_server.h"
#include "pps.h"
#include "pps_diagnostics.h"
#include "timebase.h"
#include "web_assets.h"

#define WEB_SERVER_PORT 80
#define WEB_SERVER_TASK_PRIORITY 3
#define WEB_SERVER_STACK_SIZE 6144
#define WEB_SERVER_MAX_OPEN_SOCKETS 5
#define WEB_FORMAT_BUFFER_SIZE 512
#define SEND_LITERAL(request, literal) \
    httpd_resp_send_chunk((request), (literal), sizeof(literal) - 1U)

typedef struct {
    char project[33];
    char version[33];
    char build_date[17];
    char build_time[17];
    char idf_version[33];
    char chip[48];
    char reset_reason[24];
    uint32_t flash_size;
} web_system_info_t;

static const char *TAG = "clock2-web";
static httpd_handle_t server;
static web_system_info_t system_info;

static const char *reset_reason_name(esp_reset_reason_t reason)
{
    switch (reason) {
    case ESP_RST_POWERON: return "power-on";
    case ESP_RST_EXT: return "external-pin";
    case ESP_RST_SW: return "software";
    case ESP_RST_PANIC: return "panic";
    case ESP_RST_INT_WDT: return "interrupt-watchdog";
    case ESP_RST_TASK_WDT: return "task-watchdog";
    case ESP_RST_WDT: return "watchdog";
    case ESP_RST_DEEPSLEEP: return "deep-sleep";
    case ESP_RST_BROWNOUT: return "brownout";
    case ESP_RST_SDIO: return "sdio";
    case ESP_RST_USB: return "usb";
    case ESP_RST_JTAG: return "jtag";
    case ESP_RST_EFUSE: return "efuse";
    case ESP_RST_PWR_GLITCH: return "power-glitch";
    case ESP_RST_CPU_LOCKUP: return "cpu-lockup";
    default: return "unknown";
    }
}

static esp_err_t send_chunkf(httpd_req_t *request, const char *format, ...)
{
    char buffer[WEB_FORMAT_BUFFER_SIZE];
    va_list arguments;
    va_start(arguments, format);
    const int length = vsnprintf(buffer, sizeof(buffer), format, arguments);
    va_end(arguments);

    if (length < 0 || (size_t)length >= sizeof(buffer)) {
        return ESP_ERR_INVALID_SIZE;
    }
    return httpd_resp_send_chunk(request, buffer, (ssize_t)length);
}

static esp_err_t send_json_string(httpd_req_t *request, const char *value)
{
    ESP_RETURN_ON_ERROR(
        SEND_LITERAL(request, "\""), TAG, "JSON quote failed");

    char chunk[64];
    size_t used = 0;
    const unsigned char *cursor = (const unsigned char *)value;
    while (*cursor != '\0') {
        char escaped[7];
        size_t escaped_length;
        if (*cursor == '\"' || *cursor == '\\') {
            escaped[0] = '\\';
            escaped[1] = (char)*cursor;
            escaped_length = 2;
        } else if (*cursor == '\b') {
            memcpy(escaped, "\\b", 2);
            escaped_length = 2;
        } else if (*cursor == '\f') {
            memcpy(escaped, "\\f", 2);
            escaped_length = 2;
        } else if (*cursor == '\n') {
            memcpy(escaped, "\\n", 2);
            escaped_length = 2;
        } else if (*cursor == '\r') {
            memcpy(escaped, "\\r", 2);
            escaped_length = 2;
        } else if (*cursor == '\t') {
            memcpy(escaped, "\\t", 2);
            escaped_length = 2;
        } else if (*cursor < 0x20U) {
            const int written = snprintf(
                escaped, sizeof(escaped), "\\u%04x", *cursor);
            if (written != 6) {
                return ESP_FAIL;
            }
            escaped_length = 6;
        } else {
            escaped[0] = (char)*cursor;
            escaped_length = 1;
        }

        if (used + escaped_length > sizeof(chunk)) {
            ESP_RETURN_ON_ERROR(
                httpd_resp_send_chunk(request, chunk, (ssize_t)used),
                TAG,
                "JSON string chunk failed");
            used = 0;
        }
        memcpy(chunk + used, escaped, escaped_length);
        used += escaped_length;
        cursor++;
    }

    if (used > 0) {
        ESP_RETURN_ON_ERROR(
            httpd_resp_send_chunk(request, chunk, (ssize_t)used),
            TAG,
            "JSON string final chunk failed");
    }
    return SEND_LITERAL(request, "\"");
}

static esp_err_t send_fixed_number(
    httpd_req_t *request,
    int32_t scaled_value,
    int32_t scale,
    unsigned decimals)
{
    const int64_t value = scaled_value;
    const uint64_t magnitude = value < 0
                                   ? (uint64_t)(-value)
                                   : (uint64_t)value;
    return send_chunkf(
        request,
        "%s%" PRIu64 ".%0*" PRIu64,
        value < 0 ? "-" : "",
        magnitude / (uint32_t)scale,
        (int)decimals,
        magnitude % (uint32_t)scale);
}

static void format_ipv4(uint32_t address, bool valid, char output[16])
{
    if (!valid) {
        memcpy(output, "unavailable", 12);
        return;
    }
    const ip4_addr_t ip = {.addr = address};
    if (ip4addr_ntoa_r(&ip, output, 16) == NULL) {
        memcpy(output, "unavailable", 12);
    }
}

static void format_mac(const uint8_t mac[6], char output[18])
{
    snprintf(
        output,
        18,
        "%02X:%02X:%02X:%02X:%02X:%02X",
        mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
}

static esp_err_t html_handler(httpd_req_t *request, const char *html)
{
    httpd_resp_set_type(request, "text/html; charset=utf-8");
    httpd_resp_set_hdr(request, "Cache-Control", "no-cache");
    return httpd_resp_send(request, html, HTTPD_RESP_USE_STRLEN);
}

static esp_err_t status_page_handler(httpd_req_t *request)
{
    return html_handler(request, clock2_status_html);
}

static esp_err_t advanced_page_handler(httpd_req_t *request)
{
    return html_handler(request, clock2_advanced_html);
}

static esp_err_t send_timing_json(
    httpd_req_t *request,
    const nmea_sentence_timing_snapshot_t *timing)
{
    return send_chunkf(
        request,
        "{\"n\":%" PRIu64 ",\"mean_us\":%" PRId64
        ",\"min_us\":%" PRId64 ",\"max_us\":%" PRId64 "}",
        timing->sample_count,
        timing->mean_delay_us,
        timing->minimum_delay_us,
        timing->maximum_delay_us);
}

static esp_err_t status_json_handler(httpd_req_t *request)
{
    ethernet_snapshot_t ethernet;
    nmea_status_snapshot_t gps;
    pps_status_snapshot_t pps;
    pps_diagnostics_snapshot_t pps_diagnostics;
    timebase_status_snapshot_t timebase;
    timebase_config_snapshot_t timebase_config;
    ntp_server_snapshot_t ntp;
    gps_uart_config_snapshot_t uart;

    (void)ethernet_get_snapshot(&ethernet);
    nmea_timing_get_snapshot(&gps);
    pps_get_status_snapshot(&pps);
    pps_diagnostics_get_snapshot(&pps_diagnostics);
    timebase_get_status_snapshot(&timebase);
    timebase_get_config_snapshot(&timebase_config);
    ntp_server_get_snapshot(&ntp);
    gps_uart_get_config_snapshot(&uart);

    char utc[24] = "";
    if (timebase.valid) {
        (void)timebase_format_unix_utc(
            timebase.unix_seconds, utc, sizeof(utc));
    }
    char ip[16];
    char netmask[16];
    char gateway[16];
    char mac[18];
    format_ipv4(ethernet.ipv4, ethernet.has_ipv4, ip);
    format_ipv4(ethernet.netmask, ethernet.has_ipv4, netmask);
    format_ipv4(ethernet.gateway, ethernet.has_ipv4, gateway);
    format_mac(ethernet.mac, mac);

    httpd_resp_set_type(request, "application/json");
    httpd_resp_set_hdr(request, "Cache-Control", "no-store");

    ESP_RETURN_ON_ERROR(send_chunkf(
        request,
        "{\"time\":{\"valid\":%s,\"utc\":",
        timebase.valid ? "true" : "false"), TAG, "JSON time failed");
    if (timebase.valid) {
        ESP_RETURN_ON_ERROR(send_json_string(request, utc), TAG, "UTC failed");
    } else {
        ESP_RETURN_ON_ERROR(SEND_LITERAL(request, "null"), TAG, "UTC null failed");
    }
    ESP_RETURN_ON_ERROR(send_chunkf(
        request,
        ",\"source\":\"%s\",\"pps_count\":%" PRIu32
        ",\"pps_age_us\":%" PRId64 ",\"gnss_age_us\":%" PRId64
        ",\"accepted\":%" PRIu64 ",\"rejected\":%" PRIu64 "},",
        timebase.source,
        timebase.pps_count,
        timebase.pps_age_us,
        timebase.gnss_label_age_us,
        timebase.accepted_associations,
        timebase.rejected_associations), TAG, "JSON time fields failed");

    ESP_RETURN_ON_ERROR(send_chunkf(
        request,
        "\"gps\":{\"fix\":%s,\"position_valid\":%s,\"satellites\":%u,\"hdop\":",
        gps.fix_valid ? "true" : "false",
        gps.position_valid ? "true" : "false",
        (unsigned)gps.satellites), TAG, "JSON GPS failed");
    if (gps.hdop_milli >= 0) {
        ESP_RETURN_ON_ERROR(send_fixed_number(request, gps.hdop_milli, 1000, 3), TAG, "HDOP failed");
    } else {
        ESP_RETURN_ON_ERROR(SEND_LITERAL(request, "null"), TAG, "HDOP null failed");
    }
    ESP_RETURN_ON_ERROR(SEND_LITERAL(request, ",\"latitude\":"), TAG, "Latitude key failed");
    if (gps.position_valid) {
        ESP_RETURN_ON_ERROR(send_fixed_number(request, gps.latitude_e7, 10000000, 7), TAG, "Latitude failed");
    } else {
        ESP_RETURN_ON_ERROR(SEND_LITERAL(request, "null"), TAG, "Latitude null failed");
    }
    ESP_RETURN_ON_ERROR(SEND_LITERAL(request, ",\"longitude\":"), TAG, "Longitude key failed");
    if (gps.position_valid) {
        ESP_RETURN_ON_ERROR(send_fixed_number(request, gps.longitude_e7, 10000000, 7), TAG, "Longitude failed");
    } else {
        ESP_RETURN_ON_ERROR(SEND_LITERAL(request, "null"), TAG, "Longitude null failed");
    }
    ESP_RETURN_ON_ERROR(SEND_LITERAL(request, ",\"altitude_m\":"), TAG, "Altitude key failed");
    if (gps.position_valid) {
        ESP_RETURN_ON_ERROR(send_fixed_number(request, gps.altitude_mm, 1000, 3), TAG, "Altitude failed");
    } else {
        ESP_RETURN_ON_ERROR(SEND_LITERAL(request, "null"), TAG, "Altitude null failed");
    }
    ESP_RETURN_ON_ERROR(send_chunkf(
        request,
        ",\"last_valid_age_us\":%" PRId64 ",\"timing\":{\"gga\":",
        gps.last_valid_nmea_age_us), TAG, "GPS age failed");
    ESP_RETURN_ON_ERROR(send_timing_json(request, &gps.gga), TAG, "GGA timing failed");
    ESP_RETURN_ON_ERROR(SEND_LITERAL(request, ",\"rmc\":"), TAG, "RMC key failed");
    ESP_RETURN_ON_ERROR(send_timing_json(request, &gps.rmc), TAG, "RMC timing failed");
    ESP_RETURN_ON_ERROR(SEND_LITERAL(request, ",\"zda\":"), TAG, "ZDA key failed");
    ESP_RETURN_ON_ERROR(send_timing_json(request, &gps.zda), TAG, "ZDA timing failed");
    ESP_RETURN_ON_ERROR(SEND_LITERAL(request, "}},"), TAG, "GPS close failed");

    ESP_RETURN_ON_ERROR(send_chunkf(
        request,
        "\"network\":{\"running\":%s,\"link\":%s,\"hostname\":\"%s\",\"ip\":\"%s\",\"mac\":\"%s\",\"netmask\":\"%s\",\"gateway\":\"%s\"},",
        ethernet.running ? "true" : "false",
        ethernet.link_up ? "true" : "false",
        clock2_mdns_hostname(), ip, mac, netmask, gateway), TAG, "Network JSON failed");

    ESP_RETURN_ON_ERROR(send_chunkf(
        request,
        "\"ntp\":{\"running\":%s,\"port\":%u,\"stratum\":%u,\"precision\":%d,\"reference\":\"%s\",\"received\":%" PRIu64 ",\"transmitted\":%" PRIu64 ",\"ignored\":%" PRIu64 ",\"invalid_timebase\":%" PRIu64 "},",
        ntp.running ? "true" : "false",
        ntp.port,
        ntp.stratum,
        ntp.precision,
        ntp.reference_id,
        ntp.received_packets,
        ntp.transmitted_packets,
        ntp.ignored_packets,
        ntp.invalid_timebase_requests), TAG, "NTP JSON failed");

    ESP_RETURN_ON_ERROR(send_chunkf(
        request,
        "\"pps\":{\"valid\":%s,\"count\":%" PRIu32 ",\"age_us\":%" PRId64 ",\"last_interval_us\":%" PRId64 ",\"interval_samples\":%" PRIu64 ",\"mean_interval_us\":%" PRId64 ",\"min_interval_us\":%" PRId64 ",\"max_interval_us\":%" PRId64 ",\"selected_edge\":\"%s\",\"pulse_width_us\":",
        pps.valid ? "true" : "false",
        pps.count,
        pps.age_us,
        pps.last_interval_us,
        pps.interval_samples,
        pps.mean_interval_us,
        pps.minimum_interval_us,
        pps.maximum_interval_us,
        pps_diagnostics.selected_edge), TAG, "PPS JSON failed");
    if (pps_diagnostics.pulse_width_valid) {
        ESP_RETURN_ON_ERROR(send_chunkf(request, "%" PRId64, pps_diagnostics.pulse_width_us), TAG, "Pulse width failed");
    } else {
        ESP_RETURN_ON_ERROR(SEND_LITERAL(request, "null"), TAG, "Pulse null failed");
    }
    ESP_RETURN_ON_ERROR(SEND_LITERAL(request, "},"), TAG, "PPS close failed");

    ESP_RETURN_ON_ERROR(send_chunkf(
        request,
        "\"system\":{\"uptime_s\":%" PRId64 ",\"free_heap\":%" PRIu32 ",\"minimum_free_heap\":%" PRIu32 ",\"firmware\":",
        esp_timer_get_time() / 1000000,
        esp_get_free_heap_size(),
        esp_get_minimum_free_heap_size()), TAG, "System JSON failed");
    ESP_RETURN_ON_ERROR(send_json_string(request, system_info.version), TAG, "Version failed");
    ESP_RETURN_ON_ERROR(SEND_LITERAL(request, ",\"project\":"), TAG, "Project key failed");
    ESP_RETURN_ON_ERROR(send_json_string(request, system_info.project), TAG, "Project failed");
    ESP_RETURN_ON_ERROR(SEND_LITERAL(request, ",\"idf\":"), TAG, "IDF key failed");
    ESP_RETURN_ON_ERROR(send_json_string(request, system_info.idf_version), TAG, "IDF failed");
    ESP_RETURN_ON_ERROR(SEND_LITERAL(request, ",\"build_date\":"), TAG, "Date key failed");
    ESP_RETURN_ON_ERROR(send_json_string(request, system_info.build_date), TAG, "Date failed");
    ESP_RETURN_ON_ERROR(SEND_LITERAL(request, ",\"build_time\":"), TAG, "Time key failed");
    ESP_RETURN_ON_ERROR(send_json_string(request, system_info.build_time), TAG, "Build time failed");
    ESP_RETURN_ON_ERROR(SEND_LITERAL(request, ",\"chip\":"), TAG, "Chip key failed");
    ESP_RETURN_ON_ERROR(send_json_string(request, system_info.chip), TAG, "Chip failed");
    ESP_RETURN_ON_ERROR(SEND_LITERAL(request, ",\"reset_reason\":"), TAG, "Reset key failed");
    ESP_RETURN_ON_ERROR(send_json_string(request, system_info.reset_reason), TAG, "Reset failed");
    ESP_RETURN_ON_ERROR(send_chunkf(request, ",\"flash_size\":%" PRIu32 "},", system_info.flash_size), TAG, "Flash failed");

    ESP_RETURN_ON_ERROR(send_chunkf(
        request,
        "\"diagnostics\":{\"ntp_path\":%s,\"pps_path\":%s,\"raw_nmea\":%s},",
        NTP_PATH_DIAGNOSTICS ? "true" : "false",
        PPS_PATH_DIAGNOSTICS ? "true" : "false",
        uart.raw_dump_enabled ? "true" : "false"), TAG, "Diagnostics JSON failed");

    ESP_RETURN_ON_ERROR(send_chunkf(
        request,
        "\"config\":{\"uart\":%d,\"gps_rx_gpio\":%d,\"gps_baud\":%d,\"pps_gpio\":%d,\"eth_mosi\":%d,\"eth_miso\":%d,\"eth_sclk\":%d,\"eth_cs\":%d,\"eth_int\":%d,\"eth_reset\":%d,\"eth_spi_clock_hz\":%" PRIu32 ",\"association_min_us\":%" PRId64 ",\"association_max_us\":%" PRId64 ",\"pps_timeout_us\":%" PRId64 ",\"gnss_timeout_us\":%" PRId64 "}}",
        uart.uart_num,
        uart.rx_gpio,
        uart.baud_rate,
        pps_get_gpio_num(),
        ethernet.mosi_gpio,
        ethernet.miso_gpio,
        ethernet.sclk_gpio,
        ethernet.cs_gpio,
        ethernet.int_gpio,
        ethernet.reset_gpio,
        ethernet.spi_clock_hz,
        timebase_config.association_min_us,
        timebase_config.association_max_us,
        timebase_config.pps_timeout_us,
        timebase_config.gnss_timeout_us), TAG, "Config JSON failed");

    return httpd_resp_send_chunk(request, NULL, 0);
}

static esp_err_t health_handler(httpd_req_t *request)
{
    ethernet_snapshot_t ethernet;
    timebase_status_snapshot_t timebase;
    ntp_server_snapshot_t ntp;
    (void)ethernet_get_snapshot(&ethernet);
    timebase_get_status_snapshot(&timebase);
    ntp_server_get_snapshot(&ntp);

    httpd_resp_set_type(request, "text/plain; charset=utf-8");
    httpd_resp_set_hdr(request, "Cache-Control", "no-store");
    if (!ethernet.running) {
        httpd_resp_set_status(request, "503 Service Unavailable");
        return httpd_resp_sendstr(request, "ethernet-not-running");
    }
    if (!timebase.valid) {
        httpd_resp_set_status(request, "503 Service Unavailable");
        return httpd_resp_sendstr(request, "timebase-invalid");
    }
    if (!ntp.running) {
        httpd_resp_set_status(request, "503 Service Unavailable");
        return httpd_resp_sendstr(request, "ntp-not-running");
    }
    return httpd_resp_sendstr(request, "ok");
}

static esp_err_t not_found_handler(
    httpd_req_t *request,
    httpd_err_code_t error)
{
    (void)error;
    httpd_resp_set_status(request, "404 Not Found");
    httpd_resp_set_type(request, "text/plain; charset=utf-8");
    return httpd_resp_sendstr(request, "Clock 2: not found\n");
}

static void initialize_system_info(void)
{
    const esp_app_desc_t *application = esp_app_get_description();
    snprintf(system_info.project, sizeof(system_info.project), "%s", application->project_name);
    snprintf(system_info.version, sizeof(system_info.version), "%s", application->version);
    snprintf(system_info.build_date, sizeof(system_info.build_date), "%s", application->date);
    snprintf(system_info.build_time, sizeof(system_info.build_time), "%s", application->time);
    snprintf(system_info.idf_version, sizeof(system_info.idf_version), "%s", esp_get_idf_version());

    esp_chip_info_t chip;
    esp_chip_info(&chip);
    snprintf(
        system_info.chip,
        sizeof(system_info.chip),
        "%s rev %u.%u, %u cores",
        CONFIG_IDF_TARGET,
        chip.revision / 100U,
        chip.revision % 100U,
        chip.cores);
    snprintf(
        system_info.reset_reason,
        sizeof(system_info.reset_reason),
        "%s",
        reset_reason_name(esp_reset_reason()));
    if (esp_flash_get_size(NULL, &system_info.flash_size) != ESP_OK) {
        system_info.flash_size = 0;
    }
}

esp_err_t web_server_start(void)
{
    if (server != NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    initialize_system_info();

    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.server_port = WEB_SERVER_PORT;
    config.task_priority = WEB_SERVER_TASK_PRIORITY;
    config.stack_size = WEB_SERVER_STACK_SIZE;
    config.max_open_sockets = WEB_SERVER_MAX_OPEN_SOCKETS;
    config.max_uri_handlers = 8;
    config.lru_purge_enable = true;
    config.recv_wait_timeout = 5;
    config.send_wait_timeout = 5;

    ESP_RETURN_ON_ERROR(
        httpd_start(&server, &config),
        TAG,
        "HTTP server start failed");

    const httpd_uri_t routes[] = {
        {.uri = "/", .method = HTTP_GET, .handler = status_page_handler},
        {.uri = "/advanced", .method = HTTP_GET, .handler = advanced_page_handler},
        {.uri = "/api/status", .method = HTTP_GET, .handler = status_json_handler},
        {.uri = "/health", .method = HTTP_GET, .handler = health_handler},
    };
    for (size_t index = 0; index < sizeof(routes) / sizeof(routes[0]); index++) {
        const esp_err_t result = httpd_register_uri_handler(server, &routes[index]);
        if (result != ESP_OK) {
            httpd_stop(server);
            server = NULL;
            return result;
        }
    }
    const esp_err_t error_handler_result = httpd_register_err_handler(
        server, HTTPD_404_NOT_FOUND, not_found_handler);
    if (error_handler_result != ESP_OK) {
        httpd_stop(server);
        server = NULL;
        return error_handler_result;
    }

    ESP_LOGI(TAG, "Web server started on TCP port %d", WEB_SERVER_PORT);
    return ESP_OK;
}
