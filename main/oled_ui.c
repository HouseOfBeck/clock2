#include "oled_ui.h"

#include <inttypes.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "driver/gpio.h"
#include "esp_app_desc.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lwip/ip4_addr.h"

#include "app_config.h"
#include "ethernet.h"
#include "nmea_timing.h"
#include "ntp_server.h"
#include "oled_display.h"
#include "oled_ui_logic.h"
#include "timebase.h"

#define OLED_KEY0_GPIO 33
#define OLED_KEY1_GPIO 44
#define OLED_UI_TASK_PRIORITY 1
#define OLED_UI_TASK_STACK_SIZE 4096
#define OLED_UI_POLL_MS 20U
#define OLED_UI_FRAME_INTERVAL_US 1000000LL
#define OLED_UI_ROTATION_INTERVAL_US 8000000LL
#define OLED_UI_OVERLAY_INTERVAL_US 1000000LL
#define OLED_CONTRAST_100 0x6f
#define OLED_CONTRAST_35 0x28

typedef enum {
    OLED_OVERLAY_NONE = 0,
    OLED_OVERLAY_BRIGHT_100,
    OLED_OVERLAY_BRIGHT_35,
    OLED_OVERLAY_DISPLAY_OFF,
    OLED_OVERLAY_ROTATION_PAUSED,
    OLED_OVERLAY_ROTATION_AUTO,
} oled_overlay_t;

typedef struct {
    timebase_snapshot_t timebase;
    bool time_valid;
    nmea_status_snapshot_t gps;
    ntp_server_snapshot_t ntp;
    ethernet_snapshot_t ethernet;
    char hostname[22];
    char firmware[19];
    uint64_t uptime_seconds;
} oled_ui_snapshot_t;

typedef struct {
    oled_ui_page_t page;
    oled_ui_brightness_t brightness;
    bool rotation_paused;
    bool display_powered;
    bool redraw_requested;
    oled_overlay_t overlay;
    int64_t overlay_until_us;
    int64_t next_rotation_us;
    int64_t next_frame_us;
    oled_ui_debounce_t key0;
    oled_ui_debounce_t key1;
} oled_ui_state_t;

static const char *TAG = "clock2-oled-ui";
static TaskHandle_t ui_task_handle;
static bool buttons_available;

static void draw_text(int x, int y, const char *text)
{
    oled_display_draw_text(x, y, text, 1, false);
}

static void draw_compact(int x, int y, const char *text)
{
    oled_display_draw_text(x, y, text, 1, true);
}

static void draw_centered(int y, const char *text)
{
    oled_display_draw_centered_text(y, text, 1, false);
}

static void format_milli_one_decimal(
    int32_t value_milli,
    char *output,
    size_t output_size)
{
    if (value_milli < 0) {
        (void)snprintf(output, output_size, "--");
        return;
    }
    if (value_milli > 999900) {
        value_milli = 999900;
    }
    const uint32_t rounded_tenths = ((uint32_t)value_milli + 50U) / 100U;
    (void)snprintf(output, output_size, "%" PRIu32 ".%" PRIu32,
                   rounded_tenths / 10U, rounded_tenths % 10U);
}

static void format_count(uint64_t value, char output[8])
{
    if (value < 100000U) {
        (void)snprintf(output, 8, "%" PRIu64, value);
    } else if (value < 100000000U) {
        (void)snprintf(output, 8, "%" PRIu64 "K", value / 1000U);
    } else if (value < 100000000000ULL) {
        (void)snprintf(output, 8, "%" PRIu64 "M", value / 1000000U);
    } else {
        (void)snprintf(output, 8, "99999M+");
    }
}

static void format_ipv4(
    const ethernet_snapshot_t *ethernet,
    char output[16])
{
    if (!ethernet->has_ipv4) {
        (void)snprintf(output, 16, "IP --");
        return;
    }
    const ip4_addr_t address = {.addr = ethernet->ipv4};
    if (ip4addr_ntoa_r(&address, output, 16) == NULL) {
        (void)snprintf(output, 16, "IP --");
    }
}

static void collect_snapshot(oled_ui_snapshot_t *snapshot)
{
    app_config_snapshot_t config;
    memset(snapshot, 0, sizeof(*snapshot));

    snapshot->time_valid = timebase_get_snapshot(&snapshot->timebase);
    nmea_timing_get_snapshot(&snapshot->gps);
    ntp_server_get_snapshot(&snapshot->ntp);
    (void)ethernet_get_snapshot(&snapshot->ethernet);
    app_config_get_snapshot(&config);

    oled_ui_format_hostname(
        config.active_hostname,
        snapshot->hostname,
        sizeof(snapshot->hostname),
        20);
    const esp_app_desc_t *application = esp_app_get_description();
    (void)snprintf(snapshot->firmware, sizeof(snapshot->firmware),
                   "%.18s", application->version);
    snapshot->uptime_seconds = (uint64_t)(esp_timer_get_time() / 1000000LL);
}

static void render_clock_page(const oled_ui_snapshot_t *snapshot)
{
    char time_text[9] = "--:--:--";
    if (snapshot->time_valid) {
        char utc_text[24];
        if (timebase_format_unix_utc(
                snapshot->timebase.unix_seconds,
                utc_text,
                sizeof(utc_text))) {
            memcpy(time_text, &utc_text[11], 8);
            time_text[8] = '\0';
        }
    }

    oled_display_draw_text(1, 0, time_text, 2, false);
    draw_text(103, 6, "UTC");

    if (snapshot->time_valid) {
        draw_text(1, 19, "GPS LOCK");
        if (snapshot->ntp.running) {
            draw_text(70, 19, "STRATUM 1");
        } else {
            draw_text(82, 19, "NTP OFF");
        }
    } else {
        draw_text(1, 19, "TIME INVALID");
        draw_text(89, 19, "NO NTP");
    }

    if (!snapshot->time_valid) {
        draw_text(1, 31, "NO FIX");
    } else if (snapshot->gps.fix_valid) {
        char satellites[10];
        char hdop[12];
        char hdop_value[8];
        (void)snprintf(satellites, sizeof(satellites), "%u SAT",
                       (unsigned)snapshot->gps.satellites);
        format_milli_one_decimal(
            snapshot->gps.hdop_milli, hdop_value, sizeof(hdop_value));
        (void)snprintf(hdop, sizeof(hdop), "HDOP %.5s", hdop_value);
        draw_text(1, 31, satellites);
        draw_compact(70, 31, hdop);
    } else {
        draw_text(1, 31, "NO FIX");
    }

    if (snapshot->ethernet.running && snapshot->ethernet.link_up) {
        draw_centered(53, snapshot->hostname);
    } else {
        draw_centered(53, "NO LINK");
    }
}

static void render_gps_page(const oled_ui_snapshot_t *snapshot)
{
    draw_text(0, 0, "GPS");
    if (!snapshot->gps.fix_valid) {
        draw_centered(26, "NO GPS FIX");
        return;
    }

    char text[24];
    char value[12];
    draw_text(0, 11, "FIX YES");
    (void)snprintf(text, sizeof(text), "SAT %u",
                   (unsigned)snapshot->gps.satellites);
    draw_text(76, 11, text);

    format_milli_one_decimal(
        snapshot->gps.hdop_milli, value, sizeof(value));
    (void)snprintf(text, sizeof(text), "HDOP %s", value);
    draw_text(0, 23, text);
    if (snapshot->gps.position_valid) {
        const int64_t altitude_mm = snapshot->gps.altitude_mm;
        const int64_t altitude_m = altitude_mm >= 0
                                       ? (altitude_mm + 500) / 1000
                                       : -((-altitude_mm + 500) / 1000);
        (void)snprintf(text, sizeof(text), "ALT %" PRId64 "m", altitude_m);
    } else {
        (void)snprintf(text, sizeof(text), "ALT --");
    }
    draw_compact(70, 23, text);

    if (snapshot->gps.position_valid) {
        oled_ui_format_coordinate(
            snapshot->gps.latitude_e7, true, text, sizeof(text));
        draw_text(0, 39, text);
        oled_ui_format_coordinate(
            snapshot->gps.longitude_e7, false, text, sizeof(text));
        draw_text(0, 51, text);
    }
}

static void render_network_page(const oled_ui_snapshot_t *snapshot)
{
    char text[24];
    char ip[16];

    draw_text(0, 0, "NETWORK");
    draw_text(76, 0, snapshot->ethernet.running && snapshot->ethernet.link_up
                              ? "LINK UP"
                              : "NO LINK");
    draw_text(0, 14, snapshot->hostname);

    format_ipv4(&snapshot->ethernet, ip);
    draw_text(0, 27, ip);

    (void)snprintf(
        text,
        sizeof(text),
        "MAC %02X:%02X:%02X:%02X:%02X:%02X",
        snapshot->ethernet.mac[0], snapshot->ethernet.mac[1],
        snapshot->ethernet.mac[2], snapshot->ethernet.mac[3],
        snapshot->ethernet.mac[4], snapshot->ethernet.mac[5]);
    draw_compact(0, 45, text);
}

static void render_ntp_page(const oled_ui_snapshot_t *snapshot)
{
    char text[24];
    char receive_count[8];
    char transmit_count[8];
    char uptime[20];

    draw_text(0, 0, "NTP");
    if (!snapshot->time_valid) {
        draw_text(52, 0, "TIME INVALID");
    } else if (!snapshot->ntp.running) {
        draw_text(76, 0, "NTP OFF");
    } else {
        (void)snprintf(text, sizeof(text), "STRATUM %u",
                       (unsigned)snapshot->ntp.stratum);
        draw_text(64, 0, text);
    }

    format_count(snapshot->ntp.received_packets, receive_count);
    format_count(snapshot->ntp.transmitted_packets, transmit_count);
    (void)snprintf(text, sizeof(text), "RX %s", receive_count);
    draw_compact(0, 18, text);
    (void)snprintf(text, sizeof(text), "TX %s", transmit_count);
    draw_compact(68, 18, text);

    oled_ui_format_uptime(
        snapshot->uptime_seconds, uptime, sizeof(uptime));
    (void)snprintf(text, sizeof(text), "UPTIME %.11s", uptime);
    draw_text(0, 36, text);
    (void)snprintf(text, sizeof(text), "FW %.18s", snapshot->firmware);
    draw_compact(0, 51, text);
}

static void render_page(
    oled_ui_page_t page,
    const oled_ui_snapshot_t *snapshot)
{
    switch (page) {
    case OLED_UI_PAGE_GPS:
        render_gps_page(snapshot);
        break;
    case OLED_UI_PAGE_NETWORK:
        render_network_page(snapshot);
        break;
    case OLED_UI_PAGE_NTP_SYSTEM:
        render_ntp_page(snapshot);
        break;
    case OLED_UI_PAGE_CLOCK:
    default:
        render_clock_page(snapshot);
        break;
    }
}

static const char *overlay_text(oled_overlay_t overlay)
{
    switch (overlay) {
    case OLED_OVERLAY_BRIGHT_100:
        return "BRIGHT 100%";
    case OLED_OVERLAY_BRIGHT_35:
        return "BRIGHT 35%";
    case OLED_OVERLAY_DISPLAY_OFF:
        return "DISPLAY OFF";
    case OLED_OVERLAY_ROTATION_PAUSED:
        return "ROTATION PAUSED";
    case OLED_OVERLAY_ROTATION_AUTO:
        return "ROTATION AUTO";
    case OLED_OVERLAY_NONE:
    default:
        return "";
    }
}

static void set_overlay(
    oled_ui_state_t *state,
    oled_overlay_t overlay,
    int64_t now_us)
{
    state->overlay = overlay;
    state->overlay_until_us = now_us + OLED_UI_OVERLAY_INTERVAL_US;
    state->redraw_requested = true;
}

static bool handle_key0(oled_ui_state_t *state, int64_t now_us)
{
    state->next_rotation_us = now_us + OLED_UI_ROTATION_INTERVAL_US;
    state->brightness = oled_ui_next_brightness(state->brightness);

    if (state->brightness == OLED_UI_BRIGHTNESS_35) {
        if (oled_display_set_contrast(OLED_CONTRAST_35) != ESP_OK) {
            return false;
        }
        set_overlay(state, OLED_OVERLAY_BRIGHT_35, now_us);
    } else if (state->brightness == OLED_UI_BRIGHTNESS_OFF) {
        set_overlay(state, OLED_OVERLAY_DISPLAY_OFF, now_us);
    } else {
        if (!state->display_powered &&
            oled_display_set_power(true) != ESP_OK) {
            return false;
        }
        state->display_powered = true;
        if (oled_display_set_contrast(OLED_CONTRAST_100) != ESP_OK) {
            return false;
        }
        set_overlay(state, OLED_OVERLAY_BRIGHT_100, now_us);
    }
    return true;
}

static void handle_key1(oled_ui_state_t *state, int64_t now_us)
{
    state->rotation_paused =
        oled_ui_toggle_rotation_paused(state->rotation_paused);
    state->next_rotation_us = now_us + OLED_UI_ROTATION_INTERVAL_US;
    set_overlay(
        state,
        state->rotation_paused
            ? OLED_OVERLAY_ROTATION_PAUSED
            : OLED_OVERLAY_ROTATION_AUTO,
        now_us);
}

static bool expire_overlay(oled_ui_state_t *state, int64_t now_us)
{
    if (state->overlay == OLED_OVERLAY_NONE ||
        now_us < state->overlay_until_us) {
        return true;
    }

    state->overlay = OLED_OVERLAY_NONE;
    if (state->brightness == OLED_UI_BRIGHTNESS_OFF &&
        state->display_powered) {
        if (oled_display_set_power(false) != ESP_OK) {
            return false;
        }
        state->display_powered = false;
    } else {
        state->redraw_requested = true;
    }
    return true;
}

static void advance_pages(oled_ui_state_t *state, int64_t now_us)
{
    if (state->rotation_paused) {
        return;
    }
    while (now_us >= state->next_rotation_us) {
        state->page = oled_ui_next_page(state->page);
        state->next_rotation_us += OLED_UI_ROTATION_INTERVAL_US;
        state->redraw_requested = true;
    }
}

static bool render_if_needed(oled_ui_state_t *state, int64_t now_us)
{
    if (!state->display_powered ||
        (!state->redraw_requested && now_us < state->next_frame_us)) {
        return true;
    }

    oled_display_clear();
    if (state->overlay != OLED_OVERLAY_NONE) {
        oled_display_draw_centered_text(
            25, overlay_text(state->overlay), 1, false);
    } else {
        oled_ui_snapshot_t snapshot;
        collect_snapshot(&snapshot);
        render_page(state->page, &snapshot);
    }

    if (oled_display_update() != ESP_OK) {
        return false;
    }
    state->redraw_requested = false;
    state->next_frame_us = now_us + OLED_UI_FRAME_INTERVAL_US;
    return true;
}

static void oled_ui_task(void *argument)
{
    (void)argument;
    const int64_t start_us = esp_timer_get_time();
    oled_ui_state_t state = {
        .page = OLED_UI_PAGE_CLOCK,
        .brightness = OLED_UI_BRIGHTNESS_100,
        .display_powered = true,
        .redraw_requested = true,
        .next_rotation_us = start_us + OLED_UI_ROTATION_INTERVAL_US,
        .next_frame_us = start_us,
    };
    TickType_t last_wake = xTaskGetTickCount();

    while (oled_display_is_available()) {
        const int64_t now_us = esp_timer_get_time();
        if (buttons_available) {
            const bool key0_pressed = gpio_get_level(OLED_KEY0_GPIO) == 0;
            const bool key1_pressed = gpio_get_level(OLED_KEY1_GPIO) == 0;
            if (oled_ui_debounce_update(
                    &state.key0, key0_pressed, OLED_UI_POLL_MS) &&
                !handle_key0(&state, now_us)) {
                break;
            }
            if (oled_ui_debounce_update(
                    &state.key1, key1_pressed, OLED_UI_POLL_MS)) {
                handle_key1(&state, now_us);
            }
        }

        if (!expire_overlay(&state, now_us)) {
            break;
        }
        advance_pages(&state, now_us);
        if (!render_if_needed(&state, now_us)) {
            break;
        }

        vTaskDelayUntil(&last_wake, pdMS_TO_TICKS(OLED_UI_POLL_MS));
    }

    ui_task_handle = NULL;
    ESP_LOGI(TAG, "OLED UI task stopped");
    vTaskDelete(NULL);
}

esp_err_t oled_ui_start(void)
{
    if (!oled_display_is_available() || ui_task_handle != NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    const gpio_config_t button_config = {
        .pin_bit_mask = (1ULL << OLED_KEY0_GPIO) |
                        (1ULL << OLED_KEY1_GPIO),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    const esp_err_t button_result = gpio_config(&button_config);
    buttons_available = button_result == ESP_OK;
    if (!buttons_available) {
        ESP_LOGW(TAG, "OLED buttons unavailable: %s",
                 esp_err_to_name(button_result));
    }

    const BaseType_t task_result = xTaskCreate(
        oled_ui_task,
        "oled-ui",
        OLED_UI_TASK_STACK_SIZE,
        NULL,
        OLED_UI_TASK_PRIORITY,
        &ui_task_handle);
    if (task_result != pdPASS) {
        ui_task_handle = NULL;
        ESP_LOGE(TAG, "OLED unavailable: UI task creation failed");
        return ESP_ERR_NO_MEM;
    }

    ESP_LOGI(TAG, "OLED UI task started");
    return ESP_OK;
}
