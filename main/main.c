/*
 * ESP32-S3 N16R8 ESP-SR Offline Speech Recognition
 * WakeNet9 + MultiNet Chinese Command Recognition
 * INMP441 I2S V2 Digital Microphone
 *
 * Hardware: ESP32-S3 N16R8 (16MB Flash, 8MB OPI PSRAM)
 * Mic Pins: WS=39, SCK=38, SD=37
 *
 * Wake Word: 计分器 (jì fēn qì)
 * Commands: 地主加一分, 地主加两分, 玩家一加一分
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_system.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "driver/i2s_std.h"
#include "driver/gpio.h"

/* ================================================================
 *  Pin Definitions
 * ================================================================ */
#define MIC_WS     39    // INMP441 LR Clock (Word Select)
#define MIC_SCK    38    // INMP441 Serial Clock (Bit Clock)
#define MIC_SD     37    // INMP441 Serial Data

/* ================================================================
 *  Audio Configuration
 * ================================================================ */
#define SAMPLE_RATE         16000
#define I2S_PORT            I2S_NUM_0
#define I2S_DMA_BUF_COUNT   4
#define I2S_DMA_BUF_LEN     1024

/* ================================================================
 *  SR Task Configuration
 * ================================================================ */
#define SR_TASK_STACK_SIZE  16384   // Increased for AFE model loading
#define SR_TASK_PRIORITY    5
#define SR_TASK_CORE        0

static const char *TAG = "esp_sr_app";

/* ================================================================
 *  ESP-SR AFE + MultiNet Header Includes
 * ================================================================ */
#include "esp_afe_sr_models.h"
#include "esp_mn_models.h"
#include "esp_mn_iface.h"
#include "esp_afe_sr_iface.h"
#include "model_path.h"

/* ================================================================
 *  Global Handles
 * ================================================================ */
static i2s_chan_handle_t   rx_chan = NULL;
static esp_afe_sr_data_t  *afe_data = NULL;
static const esp_afe_sr_iface_t *afe_iface = NULL;
static const esp_mn_iface_t     *multinet = NULL;
static model_iface_data_t       *mn_model = NULL;    // model_iface_data_t*, NOT esp_mn_state_t*!

/* ================================================================
 *  Command ID & String Mapping (MultiNet)
 * ================================================================ */
typedef struct {
    int         cmd_id;
    const char *cmd_str;
} sr_command_t;

static const sr_command_t sr_commands[] = {
    {0, "地主加一分"},
    {1, "地主加两分"},
    {2, "玩家一加一分"},
};

#define SR_CMD_COUNT (sizeof(sr_commands) / sizeof(sr_commands[0]))

/* ================================================================
 *  I2S V2 Initialization — INMP441 Standard Mode, Master RX
 * ================================================================ */
static esp_err_t i2s_init(void)
{
    ESP_LOGI(TAG, "Initializing I2S V2 driver for INMP441...");

    /* --- Channel config --- */
    i2s_chan_config_t chan_cfg = {
        .id            = I2S_PORT,
        .role          = I2S_ROLE_MASTER,
        .dma_desc_num  = I2S_DMA_BUF_COUNT,
        .dma_frame_num = I2S_DMA_BUF_LEN,
        .auto_clear    = true,
    };
    ESP_ERROR_CHECK(i2s_new_channel(&chan_cfg, NULL, &rx_chan));

    /* --- Standard-mode config --- */
    i2s_std_config_t std_cfg = {
        .clk_cfg  = I2S_STD_CLK_DEFAULT_CONFIG(SAMPLE_RATE),
        .slot_cfg = I2S_STD_MSB_SLOT_DEFAULT_CONFIG(
                         I2S_DATA_BIT_WIDTH_16BIT,
                         I2S_SLOT_MODE_MONO),
        .gpio_cfg = {
            .mclk = I2S_GPIO_UNUSED,
            .bclk = MIC_SCK,
            .ws   = MIC_WS,
            .dout = I2S_GPIO_UNUSED,
            .din  = MIC_SD,
            .invert_flags = {
                .mclk_inv = false,
                .bclk_inv = false,
                .ws_inv   = false,
            },
        },
    };
    ESP_ERROR_CHECK(i2s_channel_init_std_mode(rx_chan, &std_cfg));

    /* --- Enable RX channel --- */
    ESP_ERROR_CHECK(i2s_channel_enable(rx_chan));

    ESP_LOGI(TAG, "I2S V2 initialized: %d Hz, WS=%d, SCK=%d, SD=%d",
             SAMPLE_RATE, MIC_WS, MIC_SCK, MIC_SD);
    return ESP_OK;
}

/* ================================================================
 *  ESP-SR AFE Initialization
 * ================================================================ */
static esp_err_t afe_init(void)
{
    ESP_LOGI(TAG, "Initializing ESP-SR AFE (WakeNet9 + MultiNet + SE + VAD)...");

    afe_config_t cfg = {
        .aec_init                    = false,
        .se_init                     = true,       // Speech Enhancement (auto-disabled for single mic)
        .ns_init                     = false,
        .vad_init                    = true,       // Voice Activity Detection
        .wakenet_init                = true,       // WakeNet9
        .agc_init                    = false,
        .wakenet_model_name          = "wn9_nihaoxiaozhi_tts", // WakeNet9 model
        .wakenet_model_name_2        = NULL,
        .wakenet_mode                = DET_MODE_2CH_90,
        .afe_mode                    = AFE_MODE_HIGH_PERF,
        .afe_type                    = AFE_TYPE_SR,
        .afe_perferred_core          = 0,
        .afe_perferred_priority      = 5,
        .afe_ringbuf_size            = 50,
        .memory_alloc_mode           = AFE_MEMORY_ALLOC_MORE_PSRAM,
        .afe_linear_gain             = 1.0,
        .agc_mode                    = AFE_AGC_MODE_WAKENET,
        .pcm_config = {
            .total_ch_num = 3,   // AFE V1: mic_num + 2
            .mic_num      = 1,
            .mic_ids       = NULL,
            .ref_num      = 0,
            .ref_ids       = NULL,
            .sample_rate  = SAMPLE_RATE,
        },
        .debug_init      = false,
        .vad_mode        = VAD_MODE_3,
        .vad_model_name  = NULL,
        .fixed_first_channel    = true,
        .fixed_output_channel   = true,
    };

    afe_iface = esp_afe_handle_from_config(&cfg);
    if (afe_iface == NULL) {
        ESP_LOGE(TAG, "AFE handle creation failed!");
        return ESP_FAIL;
    }

    /* Create AFE data handle */
    afe_data = afe_iface->create_from_config(&cfg);
    if (afe_data == NULL) {
        ESP_LOGE(TAG, "AFE data handle creation failed!");
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "AFE initialized successfully.");
    return ESP_OK;
}

/* ================================================================
 *  Multinet Initialization
 * ================================================================ */
static esp_err_t multinet_init(void)
{
    ESP_LOGI(TAG, "Initializing MultiNet command recognition...");

    multinet = esp_mn_handle_from_name("multinet3");
    if (multinet == NULL) {
        ESP_LOGE(TAG, "Failed to get MultiNet3 handle!");
        return ESP_FAIL;
    }

    /* Get models from AFE */
    srmodel_list_t *srmodels = get_static_srmodels();
    char *model_name = esp_srmodel_filter(srmodels, ESP_MN_PREFIX, ESP_MN_CHINESE);
    if (model_name == NULL) {
        ESP_LOGE(TAG, "No Chinese MultiNet model found!");
        return ESP_FAIL;
    }
    ESP_LOGI(TAG, "MultiNet model name: %s", model_name);

    mn_model = multinet->create(model_name, 6000);
    if (mn_model == NULL) {
        ESP_LOGE(TAG, "MultiNet model creation failed!");
        return ESP_FAIL;
    }

    /* Set detection threshold (float 0.0~0.9999) */
    multinet->set_det_threshold(mn_model, 0.4f);

    ESP_LOGI(TAG, "MultiNet initialized successfully.");
    return ESP_OK;
}

/* ================================================================
 *  Wake Word & Command Callback
 * ================================================================ */
static void on_wake_word_detected(void)
{
    ESP_LOGI(TAG, "🔔 WAKE WORD DETECTED: 计分器");
    printf(">>> 唤醒词检测: [计分器] — 请说出指令 <<<\n");
}

static void on_command_detected(int cmd_id)
{
    const char *cmd_text = "未知指令";
    for (size_t i = 0; i < SR_CMD_COUNT; i++) {
        if (sr_commands[i].cmd_id == cmd_id) {
            cmd_text = sr_commands[i].cmd_str;
            break;
        }
    }

    ESP_LOGI(TAG, "✅ COMMAND [ID:%d]: %s", cmd_id, cmd_text);
    printf(">>> 识别指令: [%s] (ID=%d) <<<\n", cmd_text, cmd_id);

    /* ============================================================
     *  TODO: 在此添加斗地主积分逻辑
     * ============================================================ */
    switch (cmd_id) {
    case 0:
        printf("    → 地主 +1 分\n");
        break;
    case 1:
        printf("    → 地主 +2 分\n");
        break;
    case 2:
        printf("    → 玩家一 +1 分\n");
        break;
    default:
        break;
    }
}

/* ================================================================
 *  Speech Recognition Task (Core 0)
 * ================================================================ */
static void sr_task(void *arg)
{
    ESP_LOGI(TAG, "SR task started on Core %d, stack=%d bytes",
             xPortGetCoreID(), SR_TASK_STACK_SIZE);

    /* --- Short delay to let system stabilize --- */
    vTaskDelay(pdMS_TO_TICKS(100));

    /* --- I2S init --- */
    ESP_ERROR_CHECK(i2s_init());

    /* --- AFE init (may take several seconds to load models) --- */
    ESP_ERROR_CHECK(afe_init());

    /* --- MultiNet init --- */
    ESP_ERROR_CHECK(multinet_init());

    /* --- Audio buffer --- */
    int feed_channel = afe_iface->get_feed_channel_num(afe_data);
    int feed_samples = afe_iface->get_feed_chunksize(afe_data);
    int16_t *i2s_buf = (int16_t *)malloc(feed_samples * feed_channel * sizeof(int16_t));
    if (i2s_buf == NULL) {
        ESP_LOGE(TAG, "Failed to allocate I2S buffer!");
        vTaskDelete(NULL);
        return;
    }

    ESP_LOGI(TAG, "Feed config: channels=%d, chunksize=%d samples", feed_channel, feed_samples);
    ESP_LOGI(TAG, ">>> Listening for wake word: 计分器 <<<\n");

    bool is_wake = false;
    TickType_t last_wake_ticks = 0;

    while (1) {
        /* --- Read I2S audio --- */
        size_t bytes_read = 0;
        esp_err_t ret = i2s_channel_read(rx_chan, i2s_buf,
                        feed_samples * feed_channel * sizeof(int16_t),
                        &bytes_read, portMAX_DELAY);
        if (ret != ESP_OK) {
            ESP_LOGW(TAG, "I2S read error: %s", esp_err_to_name(ret));
            continue;
        }

        /* --- Feed audio to AFE --- */
        afe_iface->feed(afe_data, i2s_buf);

        /* --- Fetch AFE result --- */
        afe_fetch_result_t *res = afe_iface->fetch(afe_data);
        if (res == NULL) {
            continue;
        }

        /* --- Check wake word --- */
        if (res->wakeup_state == WAKENET_DETECTED) {
            on_wake_word_detected();
            is_wake = true;
            last_wake_ticks = xTaskGetTickCount();

            /* Reset MultiNet state after wake */
            if (multinet != NULL && mn_model != NULL) {
                multinet->clean(mn_model);
            }
        }

        /* --- Check MultiNet commands (only after wake) --- */
        if (is_wake && res->wakeup_state != WAKENET_DETECTED) {
            /* Feed AFE output to MultiNet, returns esp_mn_state_t enum */
            esp_mn_state_t mn_state = multinet->detect(mn_model, res->data);

            if (mn_state == ESP_MN_STATE_DETECTED) {
                /* Get results via get_results(), returns esp_mn_results_t* */
                esp_mn_results_t *mn_res = multinet->get_results(mn_model);

                for (int i = 0; i < mn_res->num; i++) {
                    if (mn_res->command_id[i] >= 0) {
                        on_command_detected(mn_res->command_id[i]);
                    }
                }
            }
        }

        /* --- Timeout: auto-sleep after 15 seconds of no command --- */
        if (is_wake && ((xTaskGetTickCount() - last_wake_ticks) > pdMS_TO_TICKS(15000))) {
            ESP_LOGI(TAG, "No command in 15s, returning to wake-word listening...\n");
            is_wake = false;
            if (multinet != NULL && mn_model != NULL) {
                multinet->clean(mn_model);
            }
        }
    }

    free(i2s_buf);
    vTaskDelete(NULL);
}

/* ================================================================
 *  app_main — ESP-IDF Standard Entry
 * ================================================================ */
void app_main(void)
{
    ESP_LOGI(TAG, "========================================");
    ESP_LOGI(TAG, " ESP32-S3 ESP-SR Speech Recognition");
    ESP_LOGI(TAG, " Board: N16R8 (16MB Flash + 8MB PSRAM)");
    ESP_LOGI(TAG, " Mic:   INMP441 (WS=39, SCK=38, SD=37)");
    ESP_LOGI(TAG, " Wake:  计分器");
    ESP_LOGI(TAG, " Cmds:  地主加一分 / 地主加两分 / 玩家一加一分");
    ESP_LOGI(TAG, "========================================\n");

    /* --- Initialize NVS --- */
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES ||
        ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    /* --- Create SR task on Core 0 --- */
    TaskHandle_t sr_task_handle = NULL;
    xTaskCreatePinnedToCore(
        sr_task,             // Task function
        "sr_task",           // Task name
        SR_TASK_STACK_SIZE,  // Stack size (bytes)
        NULL,                // Parameters
        SR_TASK_PRIORITY,    // Priority
        &sr_task_handle,     // Task handle
        SR_TASK_CORE         // Core 0
    );

    if (sr_task_handle == NULL) {
        ESP_LOGE(TAG, "Failed to create SR task!");
        return;
    }

    ESP_LOGI(TAG, "SR task created successfully on Core 0.\n");

    /* Main loop idle — everything runs in sr_task */
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}
