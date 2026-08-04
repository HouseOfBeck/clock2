#include "oled_display.h"

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#define OLED_WIDTH 64
#define OLED_HEIGHT 128
#define OLED_BYTES_PER_COLUMN (OLED_HEIGHT / 8)
#define OLED_FRAMEBUFFER_SIZE (OLED_WIDTH * OLED_HEIGHT / 8)

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

static const oled_glyph_t splash_font[] = {
    {' ', {0x00, 0x00, 0x00, 0x00, 0x00}},
    {'0', {0x3e, 0x45, 0x49, 0x51, 0x3e}},
    {'1', {0x00, 0x42, 0x7f, 0x40, 0x00}},
    {'2', {0x62, 0x51, 0x49, 0x49, 0x46}},
    {'7', {0x01, 0x01, 0x71, 0x09, 0x07}},
    {'C', {0x3e, 0x41, 0x41, 0x41, 0x22}},
    {'H', {0x7f, 0x08, 0x08, 0x08, 0x7f}},
    {'K', {0x7f, 0x08, 0x14, 0x22, 0x41}},
    {'L', {0x7f, 0x40, 0x40, 0x40, 0x40}},
    {'O', {0x3e, 0x41, 0x41, 0x41, 0x3e}},
    {'S', {0x46, 0x49, 0x49, 0x49, 0x31}},
};

static const char *TAG = "clock2-oled";
static spi_device_handle_t oled_device;
static bool oled_bus_initialized;
static bool oled_available;

/*
 * Logical portrait layout: 64 columns, each containing 16 bytes from top to
 * bottom. Bit 0 is the top pixel in each eight-pixel group. This maps directly
 * to the SH1107 vertical-addressing transfer used by Waveshare's reference
 * driver, so no second framebuffer or runtime transpose is needed.
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
    /* Waveshare Pico-OLED-1.3 SH1107 initialization, in vendor order. */
    static const uint8_t commands[] = {
        0xae,             /* Display off. */
        0x00, 0x10,       /* Initial column address. */
        0xb0,             /* Page start address. */
        0xdc, 0x00,       /* Display start line. */
        0x81, 0x6f,       /* Contrast. */
        0x21,             /* Vertical memory-addressing mode. */
        0xa1,             /* Segment remap: upright, non-mirrored orientation. */
        0xc0,             /* COM scan direction: vendor orientation. */
        0xa4,             /* Use GDDRAM contents. */
        0xa6,             /* Normal, not inverted. */
        0xa8, 0x3f,       /* 1/64 multiplex for the visible panel. */
        0xd3, 0x60,       /* Select the visible 64 COM rows. */
        0xd5, 0x41,       /* Display clock divider/oscillator. */
        0xd9, 0x22,       /* Pre-charge period. */
        0xdb, 0x35,       /* VCOMH deselect level. */
        0xad, 0x8a,       /* Enable internal DC-DC converter. */
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

static void framebuffer_clear(void)
{
    memset(framebuffer, 0, sizeof(framebuffer));
}

static void framebuffer_set_pixel(int x, int y)
{
    if (x < 0 || x >= OLED_WIDTH || y < 0 || y >= OLED_HEIGHT) {
        return;
    }
    framebuffer[(size_t)x * OLED_BYTES_PER_COLUMN + (size_t)y / 8] |=
        (uint8_t)(1U << ((unsigned)y & 7U));
}

static void framebuffer_horizontal_line(int x0, int x1, int y)
{
    for (int x = x0; x <= x1; x++) {
        framebuffer_set_pixel(x, y);
    }
}

static void framebuffer_vertical_line(int x, int y0, int y1)
{
    for (int y = y0; y <= y1; y++) {
        framebuffer_set_pixel(x, y);
    }
}

static const uint8_t *glyph_for_character(char character)
{
    for (size_t index = 0;
         index < sizeof(splash_font) / sizeof(splash_font[0]);
         index++) {
        if (splash_font[index].character == character) {
            return splash_font[index].columns;
        }
    }
    return splash_font[0].columns;
}

static void framebuffer_draw_text(int x, int y, const char *text)
{
    while (*text != '\0') {
        const uint8_t *glyph = glyph_for_character(*text++);
        for (int column = 0; column < 5; column++) {
            for (int row = 0; row < 7; row++) {
                if ((glyph[column] & (1U << row)) != 0) {
                    framebuffer_set_pixel(x + column, y + row);
                }
            }
        }
        x += 6;
    }
}

static void framebuffer_draw_centered_text(int y, const char *text)
{
    const size_t length = strlen(text);
    const int width = length == 0 ? 0 : (int)(length * 6U - 1U);
    framebuffer_draw_text((OLED_WIDTH - width) / 2, y, text);
}

static void framebuffer_draw_splash(void)
{
    framebuffer_horizontal_line(0, OLED_WIDTH - 1, 0);
    framebuffer_horizontal_line(0, OLED_WIDTH - 1, OLED_HEIGHT - 1);
    framebuffer_vertical_line(0, 0, OLED_HEIGHT - 1);
    framebuffer_vertical_line(OLED_WIDTH - 1, 0, OLED_HEIGHT - 1);

    framebuffer_draw_centered_text(52, "CLOCK 2");
    framebuffer_draw_centered_text(69, "SH1107 OK");
}

static esp_err_t oled_transfer_framebuffer(void)
{
    esp_err_t result = oled_command(0xb0);
    if (result != ESP_OK) {
        return result;
    }

    for (int x = 0; x < OLED_WIDTH; x++) {
        /* Waveshare maps visible x=0..63 to SH1107 columns 63..0. */
        const uint8_t column = (uint8_t)(OLED_WIDTH - 1 - x);
        result = oled_command((uint8_t)(column & 0x0fU));
        if (result != ESP_OK) {
            return result;
        }
        result = oled_command((uint8_t)(0x10U | (column >> 4)));
        if (result != ESP_OK) {
            return result;
        }
        result = oled_data(
            &framebuffer[(size_t)x * OLED_BYTES_PER_COLUMN],
            OLED_BYTES_PER_COLUMN);
        if (result != ESP_OK) {
            return result;
        }
    }
    return ESP_OK;
}

esp_err_t oled_display_init(void)
{
    oled_available = false;
    ESP_LOGI(TAG, "OLED initializing: SH1107 64x128 %s", OLED_SPI_HOST_NAME);
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
        .max_transfer_sz = OLED_BYTES_PER_COLUMN,
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

    /* Clear the complete visible area once before drawing the static splash. */
    framebuffer_clear();
    result = oled_transfer_framebuffer();
    if (result != ESP_OK) {
        return oled_fail(result, "clear transfer failed");
    }
    framebuffer_draw_splash();
    result = oled_transfer_framebuffer();
    if (result != ESP_OK) {
        return oled_fail(result, "splash transfer failed");
    }

    oled_available = true;
    ESP_LOGI(TAG, "OLED initialized");
    return ESP_OK;
}

bool oled_display_is_available(void)
{
    return oled_available;
}
