#pragma once

#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief LCD pin configuration structure.
 * Assign these before calling lcd_st7789_init().
 */
typedef struct {
    int mosi_io;   // SPI MOSI
    int sclk_io;   // SPI SCLK
    int cs_io;     // Chip select
    int dc_io;     // Data/command
    int rst_io;    // Reset, set to -1 if not used
    int bl_io;     // Backlight, set to -1 if not used
    uint16_t width;
    uint16_t height;
    uint32_t spi_freq_hz;  // e.g. 40 * 1000 * 1000 for 40 MHz
} lcd_st7789_config_t;

/**
 * @brief Initialize the SPI bus, attach the ST7789 device and configure the
 *        display (reset sequence, register init, display on).
 *
 * @param cfg  Pointer to a filled-in configuration.
 * @return esp_err_t  ESP_OK on success.
 */
esp_err_t lcd_st7789_init(const lcd_st7789_config_t *cfg);

/**
 * @brief Fill the entire screen with a single 16-bit RGB565 colour.
 */
void lcd_st7789_fill_screen(uint16_t color);

/**
 * @brief Fill a rectangular area with a single 16-bit RGB565 colour.
 */
void lcd_st7789_fill_rect(uint16_t x, uint16_t y, uint16_t w, uint16_t h,
                          uint16_t color);

/**
 * @brief Draw a single pixel at (x, y) in RGB565 colour.
 */
void lcd_st7789_draw_pixel(uint16_t x, uint16_t y, uint16_t color);

/**
 * @brief Draw a UTF‑8 string with an internal 8×16 / 16×16 font.
 *
 * @param x, y      Top‑left corner (pixels).
 * @param str       Null‑terminated UTF‑8 string.
 * @param fg_color  Text colour (RGB565).
 * @param bg_color  Background colour (RGB565).
 */
void lcd_st7789_draw_string(uint16_t x, uint16_t y, const char *str,
                            uint16_t fg_color, uint16_t bg_color);

/**
 * @brief Return the pixel width of a UTF‑8 string when rendered with the
 *        built‑in font.
 */
uint16_t lcd_st7789_string_width(const char *str);

/**
 * @brief De-initialize the LCD (remove SPI device, free bus).
 */
void lcd_st7789_deinit(void);

#ifdef __cplusplus
}
#endif
