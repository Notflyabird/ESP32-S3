#include "app_config.h"
#include "audio_input.h"
#include "audio_player.h"
#include "esp_err.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lcd_backlight.h"
#include "lcd_st7789.h"
#include "lcd_ui.h"
#include "nvs_flash.h"
#include "pm_profile.h"
#include "pm_sleep_mgr.h"   /* L6-A: 5min 无活动 → Light-Sleep */
#include "score_log.h"
#include "scorekeeper.h"
#include "speech_recognition.h"
#include "undo_button.h"
#include "voice_player.h"

static const char *TAG = "DDZ_APP";

void app_main(void)
{
    /* ---------- NVS init (required by score_log persistence) ---------- */
    esp_err_t nvs_err = nvs_flash_init();
    if (nvs_err == ESP_ERR_NVS_NO_FREE_PAGES || nvs_err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ESP_ERROR_CHECK(nvs_flash_init());
    } else {
        ESP_ERROR_CHECK(nvs_err);
    }

    /* ---------- L2/L3 低功耗画像：PM 锁初始化 (Tickless + DFS) ----------
     * 需要在任何会调用 _acquire() 的任务启动之前完成初始化。*/
    pm_profile_init();

    /* ---------- Score log init: restore scores + history from NVS ----------
     * 必须在 LCD / scorekeeper / undo_button 之前调用：
     * 内部打开 NVS、恢复分数到 scorekeeper、恢复日志环形缓冲。
     */
    score_log_init();

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

    /* ---------- Backlight timeout management (GPIO7 BLK) ---------- */
    if (!lcd_backlight_init((int)LCD_PIN_BL)) {
        ESP_LOGE(TAG, "lcd_backlight_init failed — continue without BL control");
    }

    lcd_ui_init_page();
    {
        int s1, s2, s3, landlord;
        scorekeeper_get_scores(&s1, &s2, &s3, &landlord);
        lcd_ui_update((uint8_t)landlord, s1, s2, s3, "初始化中...");
    }

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

    {
        int s1, s2, s3, landlord;
        scorekeeper_get_scores(&s1, &s2, &s3, &landlord);
        lcd_ui_update((uint8_t)landlord, s1, s2, s3, "就绪 你好小鑫");
    }

    /* GPIO0 BOOT 按键：10s 撤销窗口，取消最近一次计分 */
    undo_button_init();

    if (!speech_recognition_start()) {
        ESP_LOGE(TAG, "Failed to create speech tasks");
    }

    /* ---------- L6-A Light-Sleep 睡眠管理器（5 分钟无活动自动睡眠）
     * 必须在 backlight / speech / undo_button 之后启动，
     * 因为它内部会调这些模块的 suspend/resume API。*/
    pm_sleep_mgr_init();
}