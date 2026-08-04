#include "oled_display.h"

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#define OLED_PANEL_WIDTH 64
#define OLED_PANEL_HEIGHT 128
#define OLED_BYTES_PER_PANEL_COLUMN (OLED_PANEL_HEIGHT / 8)
#define OLED_FRAMEBUFFER_SIZE (OLED_DISPLAY_WIDTH * OLED_DISPLAY_HEIGHT / 8)

/* W5500 uses SPI2_HOST; the stacked OLED has its own pins on SPI3_HOST. */
#define OLED_SPI_HOST SPI3_HOST
#define OLED_SPI_HOST_NAME "SPI3"
#define OLED_MOSI_GPIO 37
#define OLED_SCLK_GPIO 38
#define OLED_CS_GPIO 39
#define OLED_DC_GPIO 40
#define OLED_RESET_GPIO 36
#define OLED_SPI_CLOCK_HZ (4 * 1000 * 1000)

typedef struct {
    char character;
    uint8_t columns[5];
} oled_glyph_t;

static const oled_glyph_t oled_font[] = {
    {' ', {0x00, 0x00, 0x00, 0x00, 0x00}},
    {'%', {0x23, 0x13, 0x08, 0x64, 0x62}},
    {'+', {0x08, 0x08, 0x3e, 0x08, 0x08}},
    {',', {0x00, 0x50, 0x30, 0x00, 0x00}},
    {'-', {0x08, 0x08, 0x08, 0x08, 0x08}},
    {'.', {0x00, 0x60, 0x60, 0x00, 0x00}},
    {'/', {0x20, 0x10, 0x08, 0x04, 0x02}},
    {'0', {0x3e, 0x45, 0x49, 0x51, 0x3e}},
    {'1', {0x00, 0x42, 0x7f, 0x40, 0x00}},
    {'2', {0x62, 0x51, 0x49, 0x49, 0x46}},
    {'3', {0x22, 0x41, 0x49, 0x49, 0x36}},
    {'4', {0x18, 0x14, 0x12, 0x7f, 0x10}},
    {'5', {0x2f, 0x49, 0x49, 0x49, 0x31}},
    {'6', {0x3e, 0x49, 0x49, 0x49, 0x32}},
    {'7', {0x01, 0x01, 0x71, 0x09, 0x07}},
    {'8', {0x36, 0x49, 0x49, 0x49, 0x36}},
    {'9', {0x26, 0x49, 0x49, 0x49, 0x3e}},
    {':', {0x00, 0x36, 0x36, 0x00, 0x00}},
    {'A', {0x7e, 0x11, 0x11, 0x11, 0x7e}},
    {'B', {0x7f, 0x49, 0x49, 0x49, 0x36}},
    {'C', {0x3e, 0x41, 0x41, 0x41, 0x22}},
    {'D', {0x7f, 0x41, 0x41, 0x22, 0x1c}},
    {'E', {0x7f, 0x49, 0x49, 0x49, 0x41}},
    {'F', {0x7f, 0x09, 0x09, 0x09, 0x01}},
    {'G', {0x3e, 0x41, 0x49, 0x49, 0x7a}},
    {'H', {0x7f, 0x08, 0x08, 0x08, 0x7f}},
    {'I', {0x00, 0x41, 0x7f, 0x41, 0x00}},
    {'J', {0x20, 0x40, 0x41, 0x3f, 0x01}},
    {'K', {0x7f, 0x08, 0x14, 0x22, 0x41}},
    {'L', {0x7f, 0x40, 0x40, 0x40, 0x40}},
    {'M', {0x7f, 0x02, 0x0c, 0x02, 0x7f}},
    {'N', {0x7f, 0x04, 0x08, 0x10, 0x7f}},
    {'O', {0x3e, 0x41, 0x41, 0x41, 0x3e}},
    {'P', {0x7f, 0x09, 0x09, 0x09, 0x06}},
    {'Q', {0x3e, 0x41, 0x51, 0x21, 0x5e}},
    {'R', {0x7f, 0x09, 0x19, 0x29, 0x46}},
    {'S', {0x46, 0x49, 0x49, 0x49, 0x31}},
    {'T', {0x01, 0x01, 0x7f, 0x01, 0x01}},
    {'U', {0x3f, 0x40, 0x40, 0x40, 0x3f}},
    {'V', {0x1f, 0x20, 0x40, 0x20, 0x1f}},
    {'W', {0x3f, 0x40, 0x38, 0x40, 0x3f}},
    {'X', {0x63, 0x14, 0x08, 0x14, 0x63}},
    {'Y', {0x07, 0x08, 0x70, 0x08, 0x07}},
    {'Z', {0x61, 0x51, 0x49, 0x45, 0x43}},
    {'a', {0x20, 0x54, 0x54, 0x54, 0x78}},
    {'b', {0x7f, 0x48, 0x44, 0x44, 0x38}},
    {'c', {0x38, 0x44, 0x44, 0x44, 0x20}},
    {'d', {0x38, 0x44, 0x44, 0x48, 0x7f}},
    {'e', {0x38, 0x54, 0x54, 0x54, 0x18}},
    {'f', {0x08, 0x7e, 0x09, 0x01, 0x02}},
    {'g', {0x0c, 0x52, 0x52, 0x52, 0x3e}},
    {'h', {0x7f, 0x08, 0x04, 0x04, 0x78}},
    {'i', {0x00, 0x44, 0x7d, 0x40, 0x00}},
    {'j', {0x20, 0x40, 0x44, 0x3d, 0x00}},
    {'k', {0x7f, 0x10, 0x28, 0x44, 0x00}},
    {'l', {0x00, 0x41, 0x7f, 0x40, 0x00}},
    {'m', {0x7c, 0x04, 0x18, 0x04, 0x78}},
    {'n', {0x7c, 0x08, 0x04, 0x04, 0x78}},
    {'o', {0x38, 0x44, 0x44, 0x44, 0x38}},
    {'p', {0x7c, 0x14, 0x14, 0x14, 0x08}},
    {'q', {0x08, 0x14, 0x14, 0x18, 0x7c}},
    {'r', {0x7c, 0x08, 0x04, 0x04, 0x08}},
    {'s', {0x48, 0x54, 0x54, 0x54, 0x20}},
    {'t', {0x04, 0x3f, 0x44, 0x40, 0x20}},
    {'u', {0x3c, 0x40, 0x40, 0x20, 0x7c}},
    {'v', {0x1c, 0x20, 0x40, 0x20, 0x1c}},
    {'w', {0x3c, 0x40, 0x30, 0x40, 0x3c}},
    {'x', {0x44, 0x28, 0x10, 0x28, 0x44}},
    {'y', {0x0c, 0x50, 0x50, 0x50, 0x3c}},
    {'z', {0x44, 0x64, 0x54, 0x4c, 0x44}},
};

static const char *TAG = "clock2-oled";
static spi_device_handle_t oled_device;
static bool oled_bus_initialized;
static bool oled_available;

/*
 * Storage remains in the controller's proven portrait transfer layout:
 * 64 panel columns, each containing 16 bytes from top to bottom. Logical
 * landscape coordinates are rotated here, once, for every drawing API:
 *
 *   panel_x = 63 - logical_y
 *   panel_y = logical_x
 *
 * This is a quarter-turn (not a reflection), so the known-good A1 segment
 * remap, C0 COM direction, and 0x60 display offset remain unchanged.
 */
static uint8_t framebuffer[OLED_FRAMEBUFFER_SIZE] __attribute__((aligned(4)));

_Static_assert(sizeof(framebuffer) == 1024,
               "SH1107 visible framebuffer must be exactly 1024 bytes");

static void oled_release_resources(void)
{
    if (oled_device != NULL) {
        (void)spi_bus_remove_device(oled_device);
        oled_device = NULL;
    }
    if (oled_bus_initialized) {
        (void)spi_bus_free(OLED_SPI_HOST);
        oled_bus_initialized = false;
    }
}

static esp_err_t oled_fail(esp_err_t result, const char *operation)
{
    oled_available = false;
    oled_release_resources();
    ESP_LOGE(TAG, "OLED unavailable: %s: %s",
             operation, esp_err_to_name(result));
    return result;
}

static esp_err_t oled_transmit_byte(uint8_t byte)
{
    spi_transaction_t transaction = {
        .flags = SPI_TRANS_USE_TXDATA,
        .length = 8,
    };
    transaction.tx_data[0] = byte;
    return spi_device_transmit(oled_device, &transaction);
}

static esp_err_t oled_command(uint8_t command)
{
    esp_err_t result = gpio_set_level(OLED_DC_GPIO, 0);
    if (result != ESP_OK) {
        return result;
    }
    return oled_transmit_byte(command);
}

static esp_err_t oled_data(const uint8_t *data, size_t length)
{
    esp_err_t result = gpio_set_level(OLED_DC_GPIO, 1);
    if (result != ESP_OK) {
        return result;
    }
    spi_transaction_t transaction = {
        .length = length * 8,
        .tx_buffer = data,
    };
    return spi_device_transmit(oled_device, &transaction);
}

static esp_err_t oled_hardware_reset(void)
{
    esp_err_t result = gpio_set_level(OLED_RESET_GPIO, 0);
    if (result != ESP_OK) {
        return result;
    }
    vTaskDelay(pdMS_TO_TICKS(10));
    result = gpio_set_level(OLED_RESET_GPIO, 1);
    if (result != ESP_OK) {
        return result;
    }
    vTaskDelay(pdMS_TO_TICKS(100));
    return ESP_OK;
}

static esp_err_t oled_initialize_controller(void)
{
    static const uint8_t commands[] = {
        0xae, 0x00, 0x10, 0xb0, 0xdc, 0x00, 0x81, 0x6f,
        0x21, 0xa1, 0xc0, 0xa4, 0xa6, 0xa8, 0x3f, 0xd3,
        0x60, 0xd5, 0x41, 0xd9, 0x22, 0xdb, 0x35, 0xad,
        0x8a,
    };

    for (size_t index = 0; index < sizeof(commands); index++) {
        const esp_err_t result = oled_command(commands[index]);
        if (result != ESP_OK) {
            return result;
        }
    }
    vTaskDelay(pdMS_TO_TICKS(200));
    return oled_command(0xaf);
}

static void framebuffer_set_pixel(int x, int y)
{
    if (x < 0 || x >= OLED_DISPLAY_WIDTH ||
        y < 0 || y >= OLED_DISPLAY_HEIGHT) {
        return;
    }

    const int panel_x = OLED_PANEL_WIDTH - 1 - y;
    const int panel_y = x;
    framebuffer[(size_t)panel_x * OLED_BYTES_PER_PANEL_COLUMN +
                (size_t)panel_y / 8] |=
        (uint8_t)(1U << ((unsigned)panel_y & 7U));
}

static const uint8_t *glyph_for_character(char character)
{
    for (size_t index = 0;
         index < sizeof(oled_font) / sizeof(oled_font[0]);
         index++) {
        if (oled_font[index].character == character) {
            return oled_font[index].columns;
        }
    }
    return oled_font[0].columns;
}

static esp_err_t oled_transfer_framebuffer(void)
{
    esp_err_t result = oled_command(0xb0);
    if (result != ESP_OK) {
        return result;
    }

    for (int panel_x = 0; panel_x < OLED_PANEL_WIDTH; panel_x++) {
        const uint8_t column = (uint8_t)(OLED_PANEL_WIDTH - 1 - panel_x);
        result = oled_command((uint8_t)(column & 0x0fU));
        if (result != ESP_OK) {
            return result;
        }
        result = oled_command((uint8_t)(0x10U | (column >> 4)));
        if (result != ESP_OK) {
            return result;
        }
        result = oled_data(
            &framebuffer[(size_t)panel_x * OLED_BYTES_PER_PANEL_COLUMN],
            OLED_BYTES_PER_PANEL_COLUMN);
        if (result != ESP_OK) {
            return result;
        }
    }
    return ESP_OK;
}

esp_err_t oled_display_init(void)
{
    oled_available = false;
    ESP_LOGI(TAG, "OLED initializing: SH1107 128x64 landscape %s",
             OLED_SPI_HOST_NAME);
    ESP_LOGI(TAG, "OLED pins: MOSI=%d CLK=%d CS=%d DC=%d RST=%d",
             OLED_MOSI_GPIO, OLED_SCLK_GPIO, OLED_CS_GPIO,
             OLED_DC_GPIO, OLED_RESET_GPIO);

    const gpio_config_t output_config = {
        .pin_bit_mask = (1ULL << OLED_CS_GPIO) |
                        (1ULL << OLED_DC_GPIO) |
                        (1ULL << OLED_RESET_GPIO),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    esp_err_t result = gpio_config(&output_config);
    if (result != ESP_OK) {
        return oled_fail(result, "GPIO configuration failed");
    }
    if ((result = gpio_set_level(OLED_CS_GPIO, 1)) != ESP_OK ||
        (result = gpio_set_level(OLED_DC_GPIO, 0)) != ESP_OK ||
        (result = gpio_set_level(OLED_RESET_GPIO, 1)) != ESP_OK) {
        return oled_fail(result, "GPIO idle-state setup failed");
    }

    const spi_bus_config_t bus_config = {
        .mosi_io_num = OLED_MOSI_GPIO,
        .miso_io_num = -1,
        .sclk_io_num = OLED_SCLK_GPIO,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = OLED_BYTES_PER_PANEL_COLUMN,
    };
    result = spi_bus_initialize(
        OLED_SPI_HOST, &bus_config, SPI_DMA_CH_AUTO);
    if (result != ESP_OK) {
        return oled_fail(result, "SPI3 bus initialization failed");
    }
    oled_bus_initialized = true;

    const spi_device_interface_config_t device_config = {
        .mode = 3,
        .clock_speed_hz = OLED_SPI_CLOCK_HZ,
        .spics_io_num = OLED_CS_GPIO,
        .queue_size = 1,
        .flags = SPI_DEVICE_HALFDUPLEX,
    };
    result = spi_bus_add_device(
        OLED_SPI_HOST, &device_config, &oled_device);
    if (result != ESP_OK) {
        return oled_fail(result, "SH1107 SPI device setup failed");
    }
    result = oled_hardware_reset();
    if (result != ESP_OK) {
        return oled_fail(result, "hardware reset failed");
    }
    result = oled_initialize_controller();
    if (result != ESP_OK) {
        return oled_fail(result, "controller initialization failed");
    }

    oled_display_clear();
    result = oled_transfer_framebuffer();
    if (result != ESP_OK) {
        return oled_fail(result, "clear transfer failed");
    }

    oled_available = true;
    ESP_LOGI(TAG, "OLED initialized");
    return ESP_OK;
}

bool oled_display_is_available(void)
{
    return oled_available;
}

void oled_display_clear(void)
{
    memset(framebuffer, 0, sizeof(framebuffer));
}

int oled_display_text_width(const char *text, unsigned scale, bool compact)
{
    if (text == NULL || *text == '\0' || scale == 0U) {
        return 0;
    }
    const size_t length = strlen(text);
    const unsigned advance = (compact ? 5U : 6U) * scale;
    return (int)((length - 1U) * advance + 5U * scale);
}

void oled_display_draw_text(
    int x,
    int y,
    const char *text,
    unsigned scale,
    bool compact)
{
    if (text == NULL || scale == 0U) {
        return;
    }
    const int advance = (int)((compact ? 5U : 6U) * scale);
    while (*text != '\0') {
        const uint8_t *glyph = glyph_for_character(*text++);
        for (int column = 0; column < 5; column++) {
            for (int row = 0; row < 7; row++) {
                if ((glyph[column] & (1U << row)) == 0U) {
                    continue;
                }
                for (unsigned dx = 0; dx < scale; dx++) {
                    for (unsigned dy = 0; dy < scale; dy++) {
                        framebuffer_set_pixel(
                            x + column * (int)scale + (int)dx,
                            y + row * (int)scale + (int)dy);
                    }
                }
            }
        }
        x += advance;
    }
}

void oled_display_draw_centered_text(
    int y,
    const char *text,
    unsigned scale,
    bool compact)
{
    const int width = oled_display_text_width(text, scale, compact);
    oled_display_draw_text(
        (OLED_DISPLAY_WIDTH - width) / 2, y, text, scale, compact);
}

esp_err_t oled_display_update(void)
{
    if (!oled_available) {
        return ESP_ERR_INVALID_STATE;
    }
    const esp_err_t result = oled_transfer_framebuffer();
    return result == ESP_OK
               ? ESP_OK
               : oled_fail(result, "framebuffer transfer failed");
}

esp_err_t oled_display_set_contrast(uint8_t contrast)
{
    if (!oled_available) {
        return ESP_ERR_INVALID_STATE;
    }
    esp_err_t result = oled_command(0x81);
    if (result == ESP_OK) {
        result = oled_command(contrast);
    }
    return result == ESP_OK
               ? ESP_OK
               : oled_fail(result, "contrast command failed");
}

esp_err_t oled_display_set_power(bool on)
{
    if (!oled_available) {
        return ESP_ERR_INVALID_STATE;
    }
    const esp_err_t result = oled_command(on ? 0xaf : 0xae);
    return result == ESP_OK
               ? ESP_OK
               : oled_fail(result, "display power command failed");
}
