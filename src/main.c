#include "app_config.h"
#include "audio_input.h"
#include "esp_log.h"
#include "lcd_st7789.h"
#include "scorekeeper.h"
#include "speech_recognition.h"

static const char *TAG = "DDZ_APP";

/* ---------- LCD colour bar demo ---------- */
static void lcd_draw_color_bars(void)
{
    lcd_st7789_fill_screen(0x0000);                     /* black  */
    lcd_st7789_fill_rect(0, 0,   240, 80, 0xF800);      /* red    */
    lcd_st7789_fill_rect(0, 80,  240, 80, 0x07E0);      /* green  */
    lcd_st7789_fill_rect(0, 160, 240, 80, 0x001F);      /* blue   */
    lcd_st7789_fill_rect(0, 240, 240, 80, 0xFFE0);      /* yellow */
    ESP_LOGI(TAG, "LCD color bars drawn");
}

void app_main(void)
{
    /* ---------- LCD init ---------- */
    lcd_st7789_config_t lcd_cfg = {
        .mosi_io = LCD_PIN_MOSI,
        .sclk_io = LCD_PIN_SCLK,
        .cs_io   = LCD_PIN_CS,
        .dc_io   = LCD_PIN_DC,
        .rst_io  = LCD_PIN_RST,
        .bl_io   = LCD_PIN_BL,
        .width   = LCD_WIDTH,
        .height  = LCD_HEIGHT,
        .spi_freq_hz = LCD_SPI_FREQ_HZ,
    };
    ESP_ERROR_CHECK(lcd_st7789_init(&lcd_cfg));
    lcd_draw_color_bars();

    /* ---------- Audio / SR init ---------- */
    audio_input_init();

    if (!speech_recognition_init()) {
        return;
    }

    ESP_LOGI(TAG, "ESP32-S3 ESP-SR Dou Dizhu scorekeeper started");
    ESP_LOGI(TAG, "Single mic INMP441: BCLK=%d WS=%d SD=%d", APP_PIN_BCLK, APP_PIN_WS, APP_PIN_SD);
    scorekeeper_print_scores("Initial");
    speech_recognition_print_pipeline();

    if (!speech_recognition_start()) {
        ESP_LOGE(TAG, "Failed to create speech tasks");
    }
}

