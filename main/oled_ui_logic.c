#include "oled_ui_logic.h"

#include <inttypes.h>
#include <stdio.h>
#include <string.h>

oled_ui_page_t oled_ui_next_page(oled_ui_page_t page)
{
    return (oled_ui_page_t)(((unsigned)page + 1U) % OLED_UI_PAGE_COUNT);
}

oled_ui_brightness_t oled_ui_next_brightness(oled_ui_brightness_t brightness)
{
    switch (brightness) {
    case OLED_UI_BRIGHTNESS_100:
        return OLED_UI_BRIGHTNESS_35;
    case OLED_UI_BRIGHTNESS_35:
        return OLED_UI_BRIGHTNESS_OFF;
    case OLED_UI_BRIGHTNESS_OFF:
    default:
        return OLED_UI_BRIGHTNESS_100;
    }
}

bool oled_ui_toggle_rotation_paused(bool paused)
{
    return !paused;
}

bool oled_ui_debounce_update(
    oled_ui_debounce_t *state,
    bool raw_pressed,
    uint32_t elapsed_ms)
{
    if (state == NULL) {
        return false;
    }
    if (raw_pressed != state->candidate_pressed) {
        state->candidate_pressed = raw_pressed;
        state->candidate_ms = elapsed_ms;
    } else if (UINT32_MAX - state->candidate_ms < elapsed_ms) {
        state->candidate_ms = UINT32_MAX;
    } else {
        state->candidate_ms += elapsed_ms;
    }

    if (state->candidate_pressed == state->stable_pressed ||
        state->candidate_ms < OLED_UI_DEBOUNCE_MS) {
        return false;
    }

    state->stable_pressed = state->candidate_pressed;
    return state->stable_pressed;
}

void oled_ui_format_hostname(
    const char *active_hostname,
    char *output,
    size_t output_size,
    size_t maximum_characters)
{
    static const char suffix[] = ".local";
    if (output == NULL || output_size == 0U) {
        return;
    }
    output[0] = '\0';
    if (active_hostname == NULL) {
        return;
    }

    if (maximum_characters >= output_size) {
        maximum_characters = output_size - 1U;
    }
    if (maximum_characters <= sizeof(suffix) - 1U) {
        (void)snprintf(output, output_size, "%.*s",
                       (int)maximum_characters, suffix);
        return;
    }

    const size_t label_limit =
        maximum_characters - (sizeof(suffix) - 1U);
    const size_t label_length = strlen(active_hostname);
    const size_t displayed_length =
        label_length > label_limit ? label_limit : label_length;
    (void)snprintf(output, output_size, "%.*s%s",
                   (int)displayed_length, active_hostname, suffix);
}

void oled_ui_format_uptime(
    uint64_t uptime_seconds,
    char *output,
    size_t output_size)
{
    if (output == NULL || output_size == 0U) {
        return;
    }
    uint64_t days = uptime_seconds / 86400U;
    if (days > 9999U) {
        days = 9999U;
    }
    const unsigned hours = (unsigned)((uptime_seconds / 3600U) % 24U);
    const unsigned minutes = (unsigned)((uptime_seconds / 60U) % 60U);
    (void)snprintf(output, output_size, "%" PRIu64 "d %02u:%02u",
                   days, hours, minutes);
}

void oled_ui_format_coordinate(
    int32_t degrees_e7,
    bool latitude,
    char *output,
    size_t output_size)
{
    if (output == NULL || output_size == 0U) {
        return;
    }

    int64_t magnitude = degrees_e7;
    if (magnitude < 0) {
        magnitude = -magnitude;
    }
    const int64_t rounded_e5 = (magnitude + 50) / 100;
    const int64_t degrees = rounded_e5 / 100000;
    const int64_t fraction = rounded_e5 % 100000;
    const char hemisphere = latitude
                                ? (degrees_e7 < 0 ? 'S' : 'N')
                                : (degrees_e7 < 0 ? 'W' : 'E');
    (void)snprintf(
        output,
        output_size,
        "%" PRId64 ".%05" PRId64 " %c",
        degrees,
        fraction,
        hemisphere);
}
