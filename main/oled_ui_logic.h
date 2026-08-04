#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define OLED_UI_PAGE_COUNT 4U
#define OLED_UI_DEBOUNCE_MS 40U

typedef enum {
    OLED_UI_PAGE_CLOCK = 0,
    OLED_UI_PAGE_GPS,
    OLED_UI_PAGE_NETWORK,
    OLED_UI_PAGE_NTP_SYSTEM,
} oled_ui_page_t;

typedef enum {
    OLED_UI_BRIGHTNESS_100 = 0,
    OLED_UI_BRIGHTNESS_35,
    OLED_UI_BRIGHTNESS_OFF,
} oled_ui_brightness_t;

typedef struct {
    bool stable_pressed;
    bool candidate_pressed;
    uint32_t candidate_ms;
} oled_ui_debounce_t;

oled_ui_page_t oled_ui_next_page(oled_ui_page_t page);
oled_ui_brightness_t oled_ui_next_brightness(oled_ui_brightness_t brightness);
bool oled_ui_toggle_rotation_paused(bool paused);

bool oled_ui_debounce_update(
    oled_ui_debounce_t *state,
    bool raw_pressed,
    uint32_t elapsed_ms);

void oled_ui_format_hostname(
    const char *active_hostname,
    char *output,
    size_t output_size,
    size_t maximum_characters);
void oled_ui_format_uptime(
    uint64_t uptime_seconds,
    char *output,
    size_t output_size);
void oled_ui_format_coordinate(
    int32_t degrees_e7,
    bool latitude,
    char *output,
    size_t output_size);
