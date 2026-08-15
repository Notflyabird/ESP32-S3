#include "undo_button.h"

#include "driver/gpio.h"
#include "esp_err.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"

#include "lcd_backlight.h"
#include "pm_sleep_mgr.h"    /* L6-A: 查询是否正在进入 Light-Sleep（GPIO0 只唤醒不做事）*/
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
    /* k1 修复：背光灭时吞掉「本次 press + 对应的 release」，仅点亮屏幕。 */
    bool     swallow_pair      = false;
    /* k2 修复：同一次日志会话内的退出播报只允许一次，避免多条路径叠加。 */
    bool     log_spoken_exit   = false;

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
            log_spoken_exit   = false;   /* k2：新一轮会话，退出播报允许一次 */
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
                if (!log_spoken_exit) {   /* k2：本次会话内的退出播报只播一次 */
                    voice_speak_log_exit();
                    log_spoken_exit = true;
                }
                press_in_progress = false;
                last_activity_ms  = now;
            } else if (in_log_view && (now - last_activity_ms) >= AUTO_EXIT_MS) {
                ESP_LOGI(TAG, "Log view auto-exit (30s idle)");
                score_log_view_exit();
                /* 加固：若用户正好此时按着按键，清掉残留状态，避免影响下一次日志会话。
                 * 30s 自动退出是静默退出，不播 log_exit（用户没按按键触发）。*/
                press_in_progress = false;
                swallow_pair      = false;
            }
            continue;
        }

        /* ============================================================
         * k1 + L6-A：按键的唯一作用是"唤醒"时，整对 press+release 吞掉。
         *   情况 A (k1)：背光灭 → 只点亮屏幕
         *   情况 B (L6-A)：pm_sleep_mgr_is_preparing_sleep() == true
         *       → Light-Sleep 前的准备序列正在跑 / 刚唤醒还在收尾，
         *         GPIO0 这次按下只是 EXT0 唤醒的副作用（或用户"唤醒一下"），
         *         不触发撤销/翻页/语音，避免"刚醒就播一句"。
         * ============================================================ */
        bool just_wake_ls = pm_sleep_mgr_is_preparing_sleep();
        if (evt.pressed && (!lcd_backlight_is_on() || just_wake_ls)) {
            if (just_wake_ls) {
                ESP_LOGI(TAG, "L6-A Light-Sleep: GPIO%d press -> wake only (no action/voice)",
                         UNDO_GPIO);
            } else {
                ESP_LOGI(TAG, "Backlight OFF: GPIO%d press -> wake screen only (no action)",
                         UNDO_GPIO);
            }
            lcd_backlight_activity();
            swallow_pair = true;      /* 对应的 release 也要吞掉 */
            press_in_progress = false;
            continue;
        }

        if (evt.pressed) {
            if (swallow_pair) {
                /* 理论上不会走到：swallow_pair=true 时 release 才来；按下时 swallow 应已清。*/
                swallow_pair = false;
                continue;
            }

            /* ⚠️ 关键修复：事件分支必须用「实时」的日志视图状态，
             * 不能用循环开头缓存的旧 in_log_view。
             * 在「缓存 in_log_view → xQueueReceive 返回 → 处理事件」之间的窗口中，
             * 语音任务（CMD_VIEW_LOG 进入日志页 / 或其他命令退出日志页）
             * 完全可能已切换视图，旧缓存会导致分流错误：
             *   实际在日志页 → 被误判为撤销主页 → 播"没有可撤销的计分"
             *   实际在主页   → 被误判为日志页按下 → 什么动作都没有
             */
            bool real_in_log = score_log_view_is_active();

            if (!real_in_log) {
                /* 主页：按下立即撤销（10s 撤销窗口语义由 scorekeeper 内部判定）*/
                ESP_LOGI(TAG, "Undo button pressed (GPIO%d low) — HOME", UNDO_GPIO);
                lcd_backlight_activity();
                scorekeeper_undo_last();
            } else {
                /* 日志页：记录按下时刻，等释放时按持续时长分流（短按翻页 / 长按退出）*/
                ESP_LOGI(TAG, "Press recorded (GPIO%d low) — LOG view, waiting release",
                         UNDO_GPIO);
                lcd_backlight_activity();
                press_in_progress = true;
                press_start_ms    = evt.timestamp_ms;
            }
        } else {
            /* ============= 释放事件 ============= */
            if (swallow_pair) {
                /* k1：背光唤醒模式下的配对 release，直接吞掉，不触发任何逻辑/语音 */
                ESP_LOGD(TAG, "Backlight wake: swallow release event");
                swallow_pair = false;
                press_in_progress = false;
                continue;
            }

            if (press_in_progress) {
                int64_t duration = evt.timestamp_ms - press_start_ms;
                press_in_progress = false;

                /* 释放分支也用「实时」的视图状态，不用旧缓存 in_log_view。
                 * 即使按下时确实在日志页、释放时已被语音切回主页，也能正确忽略。*/
                bool real_in_log = score_log_view_is_active();

                if (!real_in_log) {
                    /* 释放时视图已在主页（中途被语音命令退出日志页）→ 静默忽略，
                     * 不播任何语音，避免重复播报或"无撤销"类提示 */
                    ESP_LOGI(TAG, "Release (%lld ms): log view already exited by voice — ignore",
                             (long long)duration);
                    lcd_backlight_activity();
                } else if (duration >= LONG_PRESS_MS) {
                    ESP_LOGI(TAG, "Long-press release (%lld ms) -> exit log view",
                             (long long)duration);
                    lcd_backlight_activity();
                    last_activity_ms = evt.timestamp_ms;
                    score_log_view_exit();
                    if (!log_spoken_exit) {  /* k2：本次会话退出播报只播一次 */
                        voice_speak_log_exit();
                        log_spoken_exit = true;
                    }
                } else {
                    ESP_LOGI(TAG, "Short-press (%lld ms) -> next page", (long long)duration);
                    lcd_backlight_activity();
                    last_activity_ms = evt.timestamp_ms;
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
