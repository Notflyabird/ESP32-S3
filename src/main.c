#include "app_config.h"
#include "audio_input.h"
#include "audio_player.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lcd_st7789.h"
#include "lcd_ui.h"
#include "scorekeeper.h"
#include "speech_recognition.h"
#include "undo_button.h"
#include "voice_player.h"

static const char *TAG = "DDZ_APP";

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

    lcd_ui_init_page();
    lcd_ui_update(1, 0, 0, 0, "Initializing...");

    /* ---------- Speaker init (I2S1, MAX98357A) ---------- */
    audio_player_init();
    audio_player_self_test();

    /* ---------- Voice player init (pre-generated assets) ---------- */
    if (!voice_player_init()) {
        ESP_LOGE(TAG, "Voice player init failed");
        return;
    }
    voice_speak_boot();

    while (voice_is_playing()) {
        vTaskDelay(pdMS_TO_TICKS(50));
    }

    /* ---------- Audio / SR init (I2S0, INMP441) ---------- */
    audio_input_init();

    if (!speech_recognition_init()) {
        return;
    }

    ESP_LOGI(TAG, "ESP32-S3 ESP-SR Dou Dizhu scorekeeper started");
    ESP_LOGI(TAG, "Single mic INMP441: BCLK=%d WS=%d SD=%d", APP_PIN_BCLK, APP_PIN_WS, APP_PIN_SD);
    ESP_LOGI(TAG, "Speaker MAX98357A: I2S1 BCLK=%d LRCLK=%d DOUT=%d SD=%d",
             SPEAKER_PIN_BCLK, SPEAKER_PIN_LRCLK, SPEAKER_PIN_DOUT, SPEAKER_PIN_SD);
    scorekeeper_print_scores("Initial");
    speech_recognition_print_pipeline();

    lcd_ui_update(0, 0, 0, 0, "Ready - NiHaoXiaoXin");

    /* GPIO0 BOOT 按键：10s 撤销窗口，取消最近一次计分 */
    undo_button_init();

    if (!speech_recognition_start()) {
        ESP_LOGE(TAG, "Failed to create speech tasks");
    }
}