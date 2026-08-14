#include "undo_button.h"

#include "driver/gpio.h"
#include "esp_err.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"

#include "lcd_backlight.h"
#include "scorekeeper.h"
#include "score_log.h"
#include "voice_player.h"

static const char *TAG = "UNDO_BTN";

#define UNDO_GPIO       GPIO_NUM_0
/* 硬件消抖：30ms 内同一按键事件忽略 */
#define DEBOUNCE_MS     30
/* 日志页交互参数 */
#define LONG_PRESS_MS   800     /* 长按阈值：≥此值退出日志页 */
#define AUTO_EXIT_MS    30000   /* 日志页无操作自动退出 */

typedef struct {
    int64_t timestamp_ms;
    bool    pressed;
} button_event_t;

static QueueHandle_t      s_btn_queue;
static volatile int64_t  s_last_isr_ms;  /* ISR 级消抖时间戳 */

static void IRAM_ATTR undo_gpio_isr(void *arg)
{
    /* ISR 级消抖：同一按键边沿在 DEBOUNCE_MS 内只响应一次 */
    int64_t now = esp_timer_get_time() / 1000;
    if (now - s_last_isr_ms < DEBOUNCE_MS) {
        return;
    }
    s_last_isr_ms = now;

    /* GPIO0 低电平 = BOOT 键被按下（接外部下拉 / 按键拉低） */
    int level = gpio_get_level(UNDO_GPIO);
    button_event_t evt = {
        .timestamp_ms = now,
        .pressed      = (level == 0),
    };
    xQueueSendFromISR(s_btn_queue, &evt, NULL);
}

static void undo_button_task(void *arg)
{
    (void)arg;
    button_event_t evt;
    bool     in_log_view       = false;
    bool     prev_in_log_view  = false;   /* 上一轮的视图状态，用于检测上升沿 */
    bool     press_in_progress = false;   /* 日志页模式下按住中（等待释放判长短按）*/
    int64_t  press_start_ms    = 0;       /* 本次按下时刻 */
    int64_t  last_activity_ms  = 0;       /* 日志页最近一次交互时刻（用于 30s 自动退出）*/

    ESP_LOGI(TAG, "GPIO%d button ready (home:undo | log:short=next long=exit 30s=auto-exit)",
             UNDO_GPIO);

    while (1) {
        in_log_view = score_log_view_is_active();

        /* ---- 检测进入日志页的上升沿：重置计时基准 ----
         * 修复：如果进入前 last_activity_ms 是 0（初始）或上一次退出时的旧值，
         * 会导致 auto_exit_remaining 为负，立即触发 30s 自动退出。
         */
        if (in_log_view && !prev_in_log_view) {
            int64_t now = esp_timer_get_time() / 1000;
            last_activity_ms  = now;
            press_in_progress = false;   /* 清残留：跨页进入前的按键中途状态 */
            ESP_LOGI(TAG, "Log view entered: reset activity timer (%lld ms)",
                     (long long)now);
        }
        prev_in_log_view = in_log_view;

        /* ---- 计算下一次 xQueueReceive 的阻塞超时（非轮询，省电）---- */
        TickType_t timeout = portMAX_DELAY;
        if (in_log_view) {
            int64_t now = esp_timer_get_time() / 1000;
            int64_t auto_exit_remaining = AUTO_EXIT_MS - (now - last_activity_ms);

            if (press_in_progress) {
                /* 按住中：同时监听长按阈值与自动退出，取较小者 */
                int64_t long_press_remaining = LONG_PRESS_MS - (now - press_start_ms);
                int64_t min = (long_press_remaining < auto_exit_remaining)
                              ? long_press_remaining : auto_exit_remaining;
                timeout = pdMS_TO_TICKS(min < 0 ? 0 : min);
            } else {
                /* 未按住：仅监听自动退出 */
                if (auto_exit_remaining <= 0) {
                    ESP_LOGI(TAG, "Log view auto-exit (30s idle)");
                    score_log_view_exit();
                    continue;
                }
                timeout = pdMS_TO_TICKS(auto_exit_remaining);
            }
        }

        if (xQueueReceive(s_btn_queue, &evt, timeout) != pdTRUE) {
            /* ---- 超时：判定长按达成 或 日志页自动退出 ---- */
            int64_t now = esp_timer_get_time() / 1000;
            if (press_in_progress && (now - press_start_ms) >= LONG_PRESS_MS) {
                ESP_LOGI(TAG, "Long-press detected (%lld ms) -> exit log view",
                         (long long)(now - press_start_ms));
                lcd_backlight_activity();
                score_log_view_exit();
                voice_speak_log_exit();
                press_in_progress = false;
                last_activity_ms  = now;
            } else if (in_log_view && (now - last_activity_ms) >= AUTO_EXIT_MS) {
                ESP_LOGI(TAG, "Log view auto-exit (30s idle)");
                score_log_view_exit();
            }
            continue;
        }

        if (evt.pressed) {
            if (!in_log_view) {
                /* 主页：按下立即撤销（保持原有 10s 窗口语义由 scorekeeper 内部判定）*/
                ESP_LOGI(TAG, "Undo button pressed (GPIO%d low)", UNDO_GPIO);
                lcd_backlight_activity();
                scorekeeper_undo_last();
            } else {
                /* 日志页：记录按下时刻，等释放时按持续时长分流 */
                press_in_progress = true;
                press_start_ms    = evt.timestamp_ms;
            }
        } else {
            /* 释放事件：仅在日志页按住中才有意义 */
            if (press_in_progress) {
                int64_t duration = evt.timestamp_ms - press_start_ms;
                press_in_progress = false;
                last_activity_ms  = evt.timestamp_ms;

                if (!score_log_view_is_active()) {
                    /* 已被其它途径退出日志页（如语音），忽略释放 */
                } else if (duration >= LONG_PRESS_MS) {
                    ESP_LOGI(TAG, "Long-press release (%lld ms) -> exit log view",
                             (long long)duration);
                    lcd_backlight_activity();
                    score_log_view_exit();
                    voice_speak_log_exit();
                } else {
                    ESP_LOGI(TAG, "Short-press (%lld ms) -> next page", (long long)duration);
                    lcd_backlight_activity();
                    score_log_view_next_page();
                }
            }
        }
    }

    vTaskDelete(NULL);
}

void undo_button_init(void)
{
    s_btn_queue = xQueueCreate(8, sizeof(button_event_t));
    if (s_btn_queue == NULL) {
        ESP_LOGE(TAG, "Failed to create button queue");
        return;
    }

    gpio_config_t cfg = {
        .pin_bit_mask = (1ULL << UNDO_GPIO),
        .mode         = GPIO_MODE_INPUT,
        .pull_up_en   = GPIO_PULLUP_ENABLE,   /* GPIO0 内部上拉，按键接地 */
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_ANYEDGE,    /* 双边沿，ISR 内过滤 */
    };
    ESP_ERROR_CHECK(gpio_config(&cfg));

    s_last_isr_ms = 0;
    gpio_install_isr_service(0);
    gpio_isr_handler_add(UNDO_GPIO, undo_gpio_isr, NULL);

    xTaskCreate(undo_button_task, "undo_btn", 3072, NULL, 6, NULL);
}
