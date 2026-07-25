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
 * @brief De-initialize the LCD (remove SPI device, free bus).
 */
void lcd_st7789_deinit(void);

#ifdef __cplusplus
}
#endif
