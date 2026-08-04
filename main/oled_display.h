#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#define OLED_DISPLAY_WIDTH 128
#define OLED_DISPLAY_HEIGHT 64

esp_err_t oled_display_init(void);
bool oled_display_is_available(void);

void oled_display_clear(void);
int oled_display_text_width(const char *text, unsigned scale, bool compact);
void oled_display_draw_text(
    int x,
    int y,
    const char *text,
    unsigned scale,
    bool compact);
void oled_display_draw_centered_text(
    int y,
    const char *text,
    unsigned scale,
    bool compact);
esp_err_t oled_display_update(void);
esp_err_t oled_display_set_contrast(uint8_t contrast);
esp_err_t oled_display_set_power(bool on);
