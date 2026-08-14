#include "pm_profile.h"

#include "esp_err.h"
#include "esp_log.h"
#include "esp_pm.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

static const char *TAG = "PM";

static esp_pm_lock_handle_t s_lock_max = NULL;   /* CPU_FREQ_MAX 锁句柄 */
static int                 s_lock_ref  = 0;      /* 嵌套计数 */
static SemaphoreHandle_t   s_mtx       = NULL;
static bool                s_inited    = false;

/* ---------- 公共 API ---------- */

void pm_profile_init(void)
{
    if (s_inited) return;

    s_mtx = xSemaphoreCreateMutex();
    if (s_mtx == NULL) {
        ESP_LOGE(TAG, "Failed to create mutex");
        return;
    }

    /* 240 MHz 最大算力锁：持有期间禁止 DFS 降到 40/80 MHz */
    esp_err_t err = esp_pm_lock_create(ESP_PM_CPU_FREQ_MAX, 0, "sr_tts_max", &s_lock_max);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_pm_lock_create failed: %s — DFS lock disabled", esp_err_to_name(err));
        s_lock_max = NULL;
    } else {
        ESP_LOGI(TAG, "PM profile ready (DFS: idle=40MHz, default=160MHz, locked=240MHz)");
    }
    s_inited = true;
}

void pm_profile_high_perf_acquire(void)
{
    if (!s_inited) pm_profile_init();
    if (s_mtx == NULL) return;

    xSemaphoreTake(s_mtx, portMAX_DELAY);
    if (s_lock_ref == 0 && s_lock_max != NULL) {
        esp_err_t err = esp_pm_lock_acquire(s_lock_max);
        if (err == ESP_OK) {
            ESP_LOGD(TAG, "acquire CPU_FREQ_MAX lock (240 MHz)");
        } else {
            ESP_LOGW(TAG, "acquire lock failed: %s", esp_err_to_name(err));
        }
    }
    s_lock_ref++;
    xSemaphoreGive(s_mtx);
}

void pm_profile_high_perf_release(void)
{
    if (!s_inited || s_mtx == NULL) return;

    xSemaphoreTake(s_mtx, portMAX_DELAY);
    if (s_lock_ref > 0) {
        s_lock_ref--;
        if (s_lock_ref == 0 && s_lock_max != NULL) {
            esp_err_t err = esp_pm_lock_release(s_lock_max);
            if (err == ESP_OK) {
                ESP_LOGD(TAG, "release CPU_FREQ_MAX lock (DFS can lower freq)");
            } else {
                ESP_LOGW(TAG, "release lock failed: %s", esp_err_to_name(err));
            }
        }
    } else {
        ESP_LOGW(TAG, "release without acquire (ref already 0)");
    }
    xSemaphoreGive(s_mtx);
}

bool pm_profile_is_high_perf(void)
{
    if (!s_inited || s_mtx == NULL) return false;
    xSemaphoreTake(s_mtx, portMAX_DELAY);
    bool held = (s_lock_ref > 0);
    xSemaphoreGive(s_mtx);
    return held;
}
