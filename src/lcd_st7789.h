#pragma once

#include <stdbool.h>
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

/**
 * @brief L5: 发送 SLPIN (0x10) 命令，关闭 ST7789 内部振荡器和面板扫描，
 *        进入 Sleep 模式。GRAM 内容不丢，5ms 后进入低功耗（省 3-6mA）。
 *        背光关闭时（方案A 超时）调用。背光亮着时不要单独调这个。
 *        再次显示前必须先调 lcd_st7789_sleep_out() 且等 120ms。
 */
void lcd_st7789_sleep_in(void);

/**
 * @brief L5: 发送 SLPOUT (0x11) 命令，退出 Sleep 模式，恢复 GRAM 扫描。
 *        ST7789 数据表要求：发命令后必须至少等待 120ms 才能再写 GRAM 或亮背光，
 *        否则画面异常（闪白、偏色、不亮）。
 *        本函数内部已包含 vTaskDelay(120) 等待，调用方不用再等。
 */
void lcd_st7789_sleep_out(void);

/**
 * @brief L5: 查询当前是否处于 Sleep 状态。
 *        所有 draw/fill 函数都会检查：如处于 Sleep，内部先自动 wake_up（sleep_out+120ms）再绘制。
 */
bool lcd_st7789_is_sleeping(void);

#ifdef __cplusplus
}
#endif
