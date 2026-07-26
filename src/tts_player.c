#include "tts_player.h"

#include <string.h>

#include "audio_player.h"
#include "esp_log.h"
#include "esp_tts.h"
#include "esp_tts_voice_xiaole.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

static const char *TAG = "TTS_PLAYER";

#define TTS_TASK_STACK_SIZE 32768
#define TTS_TASK_PRIORITY   5
#define TTS_QUEUE_LENGTH    4
#define TTS_TEXT_MAX_LEN    128

static esp_tts_handle_t s_tts = NULL;
static esp_tts_voice_t *s_voice = NULL;
static volatile bool s_playing = false;

extern volatile bool g_sr_paused;

extern const uint8_t _binary_esp_tts_voice_data_xiaole_dat_start[];
extern const uint8_t _binary_esp_tts_voice_data_xiaole_dat_end[];

static QueueHandle_t s_tts_queue = NULL;
static TaskHandle_t s_tts_task_handle = NULL;

static void tts_task(void *arg);

bool tts_player_init(void)
{
    s_voice = esp_tts_voice_set_init(&esp_tts_voice_xiaole, (void *)_binary_esp_tts_voice_data_xiaole_dat_start);
    if (s_voice == NULL) {
        ESP_LOGE(TAG, "esp_tts_voice_set_init failed");
        return false;
    }

    s_tts = esp_tts_create(s_voice);
    if (s_tts == NULL) {
        ESP_LOGE(TAG, "esp_tts_create failed");
        esp_tts_voice_set_free(s_voice);
        s_voice = NULL;
        return false;
    }

    s_tts_queue = xQueueCreate(TTS_QUEUE_LENGTH, sizeof(char) * TTS_TEXT_MAX_LEN);
    if (s_tts_queue == NULL) {
        ESP_LOGE(TAG, "Failed to create TTS queue");
        esp_tts_destroy(s_tts);
        s_tts = NULL;
        esp_tts_voice_set_free(s_voice);
        s_voice = NULL;
        return false;
    }

    xTaskCreatePinnedToCore(tts_task, "tts_task", TTS_TASK_STACK_SIZE, NULL, TTS_TASK_PRIORITY,
                            &s_tts_task_handle, 1);
    if (s_tts_task_handle == NULL) {
        ESP_LOGE(TAG, "Failed to create TTS task");
        vQueueDelete(s_tts_queue);
        s_tts_queue = NULL;
        esp_tts_destroy(s_tts);
        s_tts = NULL;
        esp_tts_voice_set_free(s_voice);
        s_voice = NULL;
        return false;
    }

    ESP_LOGI(TAG, "TTS engine ready (voice=%s)", s_voice->voice_name);
    return true;
}

void tts_play_text(const char *text)
{
    if (!text || !*text) return;

    char buf[TTS_TEXT_MAX_LEN];
    strncpy(buf, text, TTS_TEXT_MAX_LEN - 1);
    buf[TTS_TEXT_MAX_LEN - 1] = '\0';

    s_playing = true;

    if (xQueueSend(s_tts_queue, buf, pdMS_TO_TICKS(100)) != pdTRUE) {
        ESP_LOGW(TAG, "TTS queue full, skipping: %s", text);
        s_playing = false;
    } else {
        ESP_LOGI(TAG, "TTS queued: %s", text);
    }
}

bool tts_is_playing(void)
{
    return s_playing;
}

void tts_player_deinit(void)
{
    if (s_tts_task_handle) {
        vTaskDelete(s_tts_task_handle);
        s_tts_task_handle = NULL;
    }
    if (s_tts_queue) {
        vQueueDelete(s_tts_queue);
        s_tts_queue = NULL;
    }
    if (s_tts) {
        esp_tts_destroy(s_tts);
        s_tts = NULL;
    }
    if (s_voice) {
        esp_tts_voice_set_free(s_voice);
        s_voice = NULL;
    }
    ESP_LOGI(TAG, "TTS engine deinitialized");
}

static void tts_task(void *arg)
{
    ESP_LOGI(TAG, "TTS task started");

    char text[TTS_TEXT_MAX_LEN];
    while (true) {
        if (xQueueReceive(s_tts_queue, text, portMAX_DELAY) != pdTRUE) {
            continue;
        }

        s_playing = true;
        ESP_LOGI(TAG, "TTS playing: %s", text);

        g_sr_paused = true;
        vTaskDelay(pdMS_TO_TICKS(50));

        audio_set_mute(false);

        if (esp_tts_parse_chinese(s_tts, text) != 1) {
            ESP_LOGE(TAG, "TTS parse failed for: %s", text);
            goto cleanup;
        }

        size_t total_samples = 0;
        int chunk_idx = 0;
        while (true) {
            int len = 0;
            ESP_LOGI(TAG, "TTS stream_play chunk %d", chunk_idx);
            short *pcm_buf = esp_tts_stream_play(s_tts, &len, 5);
            ESP_LOGI(TAG, "TTS stream_play returned: len=%d", len);
            if (len == 0 || pcm_buf == NULL) {
                break;
            }
            ESP_LOGI(TAG, "TTS audio_play_pcm: len=%d", len);
            audio_play_pcm(pcm_buf, len);
            total_samples += len;
            chunk_idx++;
            vTaskDelay(pdMS_TO_TICKS(1));
        }

        if (total_samples > 0) {
            uint32_t duration_ms = (uint32_t)(total_samples * 1000 / 16000) + 50;
            vTaskDelay(pdMS_TO_TICKS(duration_ms));
        }

cleanup:
        audio_set_mute(true);

        g_sr_paused = false;
        vTaskDelay(pdMS_TO_TICKS(20));

        s_playing = false;
        ESP_LOGI(TAG, "TTS playback finished");
    }
}

volatile bool g_sr_paused = false;