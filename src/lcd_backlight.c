#include "lcd_backlight.h"

#include "app_config.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

static const char *TAG = "LCD_BL";

#define BL_TASK_STACK    2048
#define BL_TASK_PRIO     2
#define BL_POLL_IDLE_MS  500    /* 背光已灭时的轮询周期（省电）*/

static int               s_bl_gpio = -1;
static volatile bool     s_bl_on   = false;
static volatile int64_t  s_last_activity_ms = 0;
static SemaphoreHandle_t s_lock    = NULL;
static TaskHandle_t      s_task    = NULL;

/* ---------- 内部锁 ---------- */
static inline void bl_lock(void)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
}
static inline void bl_unlock(void)
{
    xSemaphoreGive(s_lock);
}

/* ---------- 直接 GPIO 写（锁外调用，调用方持锁或任务上下文）---------- */
static void hw_set(bool on)
{
    if (s_bl_gpio < 0) return;
    gpio_set_level((gpio_num_t)s_bl_gpio, on ? 1 : 0);
}

/* ---------- 后台任务 ---------- */
static void bl_task(void *arg)
{
    (void)arg;
    ESP_LOGI(TAG, "backlight task started (timeout=%u ms)", (unsigned)LCD_BL_TIMEOUT_MS);

    while (1) {
        bl_lock();
        int64_t last = s_last_activity_ms;
        bool    cur_on = s_bl_on;
        bl_unlock();

        int64_t now = esp_timer_get_time() / 1000;
        int64_t elapsed = now - last;
        int64_t sleep_ms;

        if (cur_on) {
            if (elapsed >= (int64_t)LCD_BL_TIMEOUT_MS) {
                /* 超时：关背光 */
                ESP_LOGI(TAG, "idle %lld ms -> backlight OFF", (long long)elapsed);
                bl_lock();
                hw_set(false);
                s_bl_on = false;
                bl_unlock();
                sleep_ms = BL_POLL_IDLE_MS;
            } else {
                /* 等剩余时间到期 */
                sleep_ms = (int64_t)LCD_BL_TIMEOUT_MS - elapsed;
                if (sleep_ms < 10) sleep_ms = 10;
            }
        } else {
            /* 背光已关：低频轮询等待 activity 唤醒 */
            sleep_ms = BL_POLL_IDLE_MS;
        }

        vTaskDelay(pdMS_TO_TICKS((uint32_t)sleep_ms));
    }
}

/* ---------- 公共 API ---------- */

bool lcd_backlight_init(int bl_gpio)
{
    if (bl_gpio < 0) {
        ESP_LOGE(TAG, "invalid BL gpio: %d", bl_gpio);
        return false;
    }
    s_bl_gpio = bl_gpio;

    s_lock = xSemaphoreCreateMutex();
    if (s_lock == NULL) {
        ESP_LOGE(TAG, "Failed to create lock");
        return false;
    }

    /* GPIO 配置：与 lcd_st7789.c 相同配置（OUTPUT，无上拉）。
     * 在此重新配置以保证此模块可独立控制，即便顺序有差异。*/
    gpio_config_t bl_conf = {
        .pin_bit_mask = (1ULL << bl_gpio),
        .mode         = GPIO_MODE_OUTPUT,
        .pull_up_en   = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_DISABLE,
    };
    ESP_ERROR_CHECK(gpio_config(&bl_conf));

    /* 初始化后立即开背光 */
    int64_t now = esp_timer_get_time() / 1000;
    bl_lock();
    hw_set(true);
    s_bl_on = true;
    s_last_activity_ms = now;
    bl_unlock();

    BaseType_t r = xTaskCreate(bl_task, "lcd_bl", BL_TASK_STACK,
                               NULL, BL_TASK_PRIO, &s_task);
    if (r != pdPASS) {
        ESP_LOGE(TAG, "Failed to create backlight task");
        return false;
    }

    ESP_LOGI(TAG, "backlight ready GPIO%d (ON, timeout=%ums)",
             bl_gpio, (unsigned)LCD_BL_TIMEOUT_MS);
    return true;
}

void lcd_backlight_on(void)
{
    if (s_lock == NULL) return;
    int64_t now = esp_timer_get_time() / 1000;
    bl_lock();
    if (!s_bl_on) {
        hw_set(true);
        s_bl_on = true;
        ESP_LOGD(TAG, "backlight ON (forced)");
    }
    s_last_activity_ms = now;
    bl_unlock();
}

void lcd_backlight_off(void)
{
    if (s_lock == NULL) return;
    bl_lock();
    if (s_bl_on) {
        hw_set(false);
        s_bl_on = false;
        ESP_LOGD(TAG, "backlight OFF (forced)");
    }
    bl_unlock();
}

void lcd_backlight_activity(void)
{
    if (s_lock == NULL) return;
    int64_t now = esp_timer_get_time() / 1000;
    bl_lock();
    s_last_activity_ms = now;
    if (!s_bl_on) {
        hw_set(true);
        s_bl_on = true;
        ESP_LOGD(TAG, "backlight ON (activity)");
    }
    bl_unlock();
}

bool lcd_backlight_is_on(void)
{
    if (s_lock == NULL) return false;
    bl_lock();
    bool on = s_bl_on;
    bl_unlock();
    return on;
}
