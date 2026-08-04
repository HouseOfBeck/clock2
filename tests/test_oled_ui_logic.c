#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "oled_ui_logic.h"

static void test_rotation(void)
{
    oled_ui_page_t page = OLED_UI_PAGE_CLOCK;
    page = oled_ui_next_page(page);
    assert(page == OLED_UI_PAGE_GPS);
    page = oled_ui_next_page(page);
    assert(page == OLED_UI_PAGE_NETWORK);
    page = oled_ui_next_page(page);
    assert(page == OLED_UI_PAGE_NTP_SYSTEM);
    page = oled_ui_next_page(page);
    assert(page == OLED_UI_PAGE_CLOCK);
}

static void test_brightness_and_pause(void)
{
    oled_ui_brightness_t brightness = OLED_UI_BRIGHTNESS_100;
    brightness = oled_ui_next_brightness(brightness);
    assert(brightness == OLED_UI_BRIGHTNESS_35);
    brightness = oled_ui_next_brightness(brightness);
    assert(brightness == OLED_UI_BRIGHTNESS_OFF);
    brightness = oled_ui_next_brightness(brightness);
    assert(brightness == OLED_UI_BRIGHTNESS_100);

    bool paused = false;
    paused = oled_ui_toggle_rotation_paused(paused);
    assert(paused);
    paused = oled_ui_toggle_rotation_paused(paused);
    assert(!paused);
}

static void test_debounce(void)
{
    oled_ui_debounce_t state = {0};

    assert(!oled_ui_debounce_update(&state, true, 20));
    assert(oled_ui_debounce_update(&state, true, 20));
    assert(!oled_ui_debounce_update(&state, true, 100));
    assert(!oled_ui_debounce_update(&state, false, 20));
    assert(!oled_ui_debounce_update(&state, false, 20));
    assert(!oled_ui_debounce_update(&state, true, 20));
    assert(oled_ui_debounce_update(&state, true, 20));

    state = (oled_ui_debounce_t){0};
    assert(!oled_ui_debounce_update(&state, true, 20));
    assert(!oled_ui_debounce_update(&state, false, 20));
    assert(!state.stable_pressed);
}

static void test_formatting(void)
{
    char text[32];
    oled_ui_format_hostname("clock2", text, sizeof(text), 20);
    assert(strcmp(text, "clock2.local") == 0);
    oled_ui_format_hostname("office-clock-long", text, sizeof(text), 12);
    assert(strcmp(text, "office.local") == 0);

    oled_ui_format_uptime(93780, text, sizeof(text));
    assert(strcmp(text, "1d 02:03") == 0);

    oled_ui_format_coordinate(360222600, true, text, sizeof(text));
    assert(strcmp(text, "36.02226 N") == 0);
    oled_ui_format_coordinate(-842129700, false, text, sizeof(text));
    assert(strcmp(text, "84.21297 W") == 0);
}

int main(void)
{
    test_rotation();
    test_brightness_and_pause();
    test_debounce();
    test_formatting();
    puts("OLED UI logic tests passed");
    return 0;
}
