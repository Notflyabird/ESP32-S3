#include "speech_recognition.h"

#include <stdlib.h>

#include "app_config.h"
#include "audio_input.h"
#include "esp_afe_config.h"
#include "esp_afe_sr_iface.h"
#include "esp_afe_sr_models.h"
#include "esp_log.h"
#include "esp_mn_iface.h"
#include "esp_mn_models.h"
#include "esp_mn_speech_commands.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lcd_backlight.h"
#include "lcd_ui.h"
#include "pm_profile.h"
#include "model_path.h"
#include "scorekeeper.h"
#include "voice_player.h"

extern volatile bool g_sr_paused;

static const char *TAG = "DDZ_SR";

/* 概率阈值：低于此值的识别结果视为不可靠，过滤不计分。
 * ESP-SR MultiNet 总会返回最高概率匹配（哪怕说的是噪声），
 * 典型正常语音 prob > 0.4，纯噪声/碰撞声 < 0.2。
 * 0.30 仅过滤明显噪声，保留模糊识别以兼顾灵敏度。*/
#define COMMAND_PROB_THRESHOLD  0.30f
/* 连续低概率次数达到此值后播放"没听清"提示 */
#define LOW_PROB_HINT_LIMIT     2

typedef struct {
    const char *language;
    const esp_mn_iface_t *iface;
    model_iface_data_t *data;
} multinet_model_t;

typedef struct {
    srmodel_list_t *models;
    const esp_afe_sr_iface_t *afe_iface;
    esp_afe_sr_data_t *afe_data;
    multinet_model_t chinese;
} speech_context_t;

static speech_context_t s_speech;

static bool configure_chinese_commands(multinet_model_t *model)
{
    ESP_ERROR_CHECK(esp_mn_commands_alloc(model->iface, model->data));

    bool ok = scorekeeper_register_commands();
    esp_mn_error_t *errors = ok ? esp_mn_commands_update() : NULL;
    if (errors != NULL) {
        ESP_LOGE(TAG, "Command update failed; check pinyin command words");
        ok = false;
    }

    esp_mn_commands_free();
    return ok;
}

static bool init_chinese_multinet(srmodel_list_t *models, multinet_model_t *model)
{
    char *model_name = esp_srmodel_filter(models, ESP_MN_PREFIX, ESP_MN_CHINESE);
    if (model_name == NULL) {
        ESP_LOGE(TAG, "Chinese MultiNet model is missing");
        return false;
    }

    model->language = "Chinese";
    model->iface = esp_mn_handle_from_name(model_name);
    model->data = model->iface != NULL ? model->iface->create(model_name, APP_COMMAND_TIMEOUT_MS) : NULL;
    if (model->data == NULL) {
        ESP_LOGE(TAG, "Failed to create Chinese MultiNet: %s", model_name);
        return false;
    }

    if (!configure_chinese_commands(model)) {
        model->iface->destroy(model->data);
        model->data = NULL;
        return false;
    }

    ESP_LOGI(TAG, "Chinese commands ready: %s", model_name);
    return true;
}

bool speech_recognition_init(void)
{
    s_speech.models = esp_srmodel_init("model");
    if (s_speech.models == NULL) {
        ESP_LOGE(TAG, "Failed to load model partition");
        return false;
    }

    afe_config_t *afe_config = afe_config_init("M", s_speech.models, AFE_TYPE_SR, AFE_MODE_LOW_COST);
    if (afe_config == NULL) {
        ESP_LOGE(TAG, "Failed to create AFE config");
        return false;
    }

    afe_config->aec_init = false;
    afe_config->se_init = false;
    afe_config->memory_alloc_mode = AFE_MEMORY_ALLOC_MORE_PSRAM;
    s_speech.afe_iface = esp_afe_handle_from_config(afe_config);
    s_speech.afe_data = s_speech.afe_iface != NULL
                            ? s_speech.afe_iface->create_from_config(afe_config)
                            : NULL;
    afe_config_free(afe_config);

    if (s_speech.afe_data == NULL ||
        !init_chinese_multinet(s_speech.models, &s_speech.chinese)) {
        ESP_LOGE(TAG, "Speech recognition initialization failed");
        return false;
    }

    return true;
}

void speech_recognition_print_pipeline(void)
{
    if (s_speech.afe_iface != NULL && s_speech.afe_data != NULL) {
        s_speech.afe_iface->print_pipeline(s_speech.afe_data);
    }
}

static void feed_task(void *arg)
{
    speech_context_t *speech = (speech_context_t *)arg;
    const int chunk = speech->afe_iface->get_feed_chunksize(speech->afe_data);
    const int channels = speech->afe_iface->get_feed_channel_num(speech->afe_data);

    if (chunk <= 0 || channels != 1) {
        ESP_LOGE(TAG, "Unsupported AFE input: chunk=%d channels=%d", chunk, channels);
        vTaskDelete(NULL);
        return;
    }

    int32_t *raw = (int32_t *)audio_input_alloc((size_t)chunk * sizeof(*raw));
    int16_t *pcm = (int16_t *)audio_input_alloc((size_t)chunk * sizeof(*pcm));
    if (raw == NULL || pcm == NULL) {
        ESP_LOGE(TAG, "Failed to allocate AFE feed buffers");
        free(raw);
        free(pcm);
        vTaskDelete(NULL);
        return;
    }

    ESP_LOGI(TAG, "AFE feed task started: chunk=%d samples", chunk);

    uint32_t stats_counter = 0;
    int32_t max_abs = 0;

    while (true) {
        while (g_sr_paused) {
            vTaskDelay(pdMS_TO_TICKS(10));
        }
        esp_err_t err = audio_input_read_pcm_chunk(raw, pcm, chunk);
        if (err == ESP_OK) {
            /* 峰值统计：每 512 chunk（~16s）打印一次，仅 DEBUG 级别 */
            for (int i = 0; i < chunk; ++i) {
                int32_t v = pcm[i] < 0 ? -pcm[i] : pcm[i];
                if (v > max_abs) max_abs = v;
            }
            stats_counter++;
            if ((stats_counter & 0x1FF) == 0) {
                ESP_LOGD(TAG, "AFE fed %u chunks, peak=%d", (unsigned)stats_counter, (int)max_abs);
                max_abs = 0;
            }
            speech->afe_iface->feed(speech->afe_data, pcm);
        } else {
            ESP_LOGW(TAG, "I2S read failed: %s", esp_err_to_name(err));
        }
    }
}

static int detect_command(multinet_model_t *model, int16_t *audio,
                          bool *timed_out, bool *rejected)
{
    *rejected = false;
    esp_mn_state_t state = model->iface->detect(model->data, audio);
    if (state == ESP_MN_STATE_TIMEOUT) {
        *timed_out = true;
        return -1;
    }
    if (state != ESP_MN_STATE_DETECTED) {
        return -1;
    }

    esp_mn_results_t *results = model->iface->get_results(model->data);
    if (results == NULL || results->num <= 0) {
        return -1;
    }

    /* 概率阈值过滤：低于阈值视为不可靠，清除状态让 MultiNet 重新检测 */
    if (results->prob[0] < COMMAND_PROB_THRESHOLD) {
        ESP_LOGW(TAG, "Command %d rejected: prob=%.3f < %.2f",
                 results->command_id[0], results->prob[0], COMMAND_PROB_THRESHOLD);
        *rejected = true;
        model->iface->clean(model->data);
        return -1;
    }

    ESP_LOGI(TAG, "%s command=%d probability=%.3f",
             model->language, results->command_id[0], results->prob[0]);
    return results->command_id[0];
}

static void detect_task(void *arg)
{
    speech_context_t *speech = (speech_context_t *)arg;
    const int afe_chunk = speech->afe_iface->get_fetch_chunksize(speech->afe_data);
    const int mn_chunk = speech->chinese.iface->get_samp_chunksize(speech->chinese.data);

    if (afe_chunk != mn_chunk) {
        ESP_LOGE(TAG, "Chunk mismatch: AFE=%d MN=%d", afe_chunk, mn_chunk);
        vTaskDelete(NULL);
        return;
    }

    bool command_session = false;
    bool timed_out = false;
    int  low_prob_count = 0;     /* 连续低概率次数 */
    ESP_LOGI(TAG, "SR detect task started, waiting for wake word (你好小鑫)");

    while (true) {
        afe_fetch_result_t *result = speech->afe_iface->fetch(speech->afe_data);
        if (result == NULL || result->ret_value == ESP_FAIL) {
            ESP_LOGW(TAG, "AFE fetch failed");
            continue;
        }

        if (result->wakeup_state == WAKENET_DETECTED) {
            ESP_LOGI(TAG, "Wake word detected! Listening for command...");
            lcd_backlight_activity();
            pm_profile_high_perf_acquire();   /* L3: 语音会话期拉 240 MHz，确保 SR 流畅 */
            int s1, s2, s3, landlord;
            scorekeeper_get_scores(&s1, &s2, &s3, &landlord);
            lcd_ui_update((uint8_t)landlord, s1, s2, s3, "我在听...");
            voice_speak_im_here();
            speech->chinese.iface->clean(speech->chinese.data);
            speech->afe_iface->disable_wakenet(speech->afe_data);
            command_session = true;
            timed_out = false;
            low_prob_count = 0;
        }

        if (!command_session) {
            continue;
        }

        bool rejected = false;
        int command = detect_command(&speech->chinese, result->data,
                                     &timed_out, &rejected);
        if (command >= 0) {
            /* 合法命令：重置计数，执行计分，会话结束 */
            lcd_backlight_activity();
            low_prob_count = 0;
            scorekeeper_apply_command(command);
            speech->chinese.iface->clean(speech->chinese.data);
            speech->afe_iface->enable_wakenet(speech->afe_data);
            command_session = false;
            pm_profile_high_perf_release();   /* L3: 会话结束，释放 240 MHz 锁 */
        } else if (rejected) {
            /* 低概率被过滤：用户说了但不可靠，也算一次活动；连续多次后提示"没听清"。
             * command_session 保持 true，所以仍然需要高算力（用户可能继续说）。*/
            lcd_backlight_activity();
            low_prob_count++;
            if (low_prob_count >= LOW_PROB_HINT_LIMIT) {
                ESP_LOGI(TAG, "Low prob x%d -> speak unclear hint", low_prob_count);
                voice_speak_command_unclear();
                low_prob_count = 0;
            }
            /* command_session 保持 true，继续等待用户重说 */
        } else if (timed_out) {
            speech->afe_iface->enable_wakenet(speech->afe_data);
            command_session = false;
            pm_profile_high_perf_release();   /* L3: 会话超时，释放 240 MHz 锁 */
            int s1, s2, s3, landlord;
            scorekeeper_get_scores(&s1, &s2, &s3, &landlord);
            lcd_ui_update((uint8_t)landlord, s1, s2, s3, "就绪 你好小鑫");
        }
    }
}

bool speech_recognition_start(void)
{
    return xTaskCreatePinnedToCore(feed_task, "afe_feed", 8192, &s_speech, 5, NULL, 0) == pdPASS &&
           xTaskCreatePinnedToCore(detect_task, "sr_detect", 12288, &s_speech, 5, NULL, 1) == pdPASS;
}

