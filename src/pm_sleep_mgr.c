#include "pm_sleep_mgr.h"

#include "app_config.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_pm.h"
#include "esp_sleep.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "lcd_backlight.h"
#include "lcd_st7789.h"
#include "pm_profile.h"
#include "speech_recognition.h"

static const char *TAG = "PM_LS";

/* ---------- 配置 ---------- */
#ifndef PM_LS_TIMEOUT_MS
#define PM_LS_TIMEOUT_MS  (5 * 60 * 1000)   /* 5 分钟无活动 → Light-Sleep */
#endif

#define PM_LS_TASK_STACK   8192             /* esp_light_sleep_start 内部需要较多栈 */
#define PM_LS_TASK_PRIO    1
#define PM_LS_POLL_IDLE_MS 500              /* 正常工作：半秒轮询一次（省电）*/
#define PM_LS_WAKE_GPIO    GPIO_NUM_0       /* 按键 GPIO：Light-Sleep GPIO 唤醒源 */

/* ---------- 模块状态 ---------- */
static SemaphoreHandle_t s_lock          = NULL;
static volatile int64_t  s_last_act_ms   = 0;   /* 最后一次用户活动时间戳 */
static volatile bool     s_preparing     = false;  /* true = 已进入睡眠序列（马上 esp_light_sleep_start）*/
/* L6-A 修复：刚从 Light-Sleep 唤醒后的"宽限期截止时间戳"（ms）。
 * 在此时刻之前，pm_sleep_mgr_is_preparing_sleep() 继续返回 true，
 * 让 undo_button 有足够的时间吞掉"触发 EXT0 唤醒的那一次 GPIO0 press+release 事件对"，
 * 否则 s_preparing 在 esp_light_sleep_start() 返回后就被清 false，
 * 而唤醒按键事件稍后才到达 undo_button_task → 不被吞 → 立刻播"没有可撤销"。*/
static volatile int64_t  s_wake_grace_until_ms = 0;
#define PM_LS_WAKE_GRACE_MS  500   /* 唤醒后的 500ms 内，GPIO0 按键一律视为"仅唤醒" */
static TaskHandle_t      s_task          = NULL;

/* ---------- 内部锁 ---------- */
static inline void ls_lock(void)   { xSemaphoreTake(s_lock, portMAX_DELAY); }
static inline void ls_unlock(void) { xSemaphoreGive(s_lock); }

/* ================================================================
 * 进入 Light-Sleep 前的准备序列
 *   顺序：SR I2S 停 → 背光关 → LCD sleep_in → 释放残留 PM 锁 → 配唤醒源
 * ================================================================ */
static void do_enter_light_sleep(void)
{
    ESP_LOGI(TAG, "=== L6-A enter Light-Sleep (wake=GPIO%d) ===", (int)PM_LS_WAKE_GPIO);

    /* (1) 停 I2S 采样 + AFE feed。
     *     AFE / MultiNet 句柄都保留在 PSRAM / 内部 RAM 中，不销毁。*/
    speech_suspend_i2s();

    /* (2) 关背光 LED（由 lcd_backlight_off 直接写 GPIO，不走超时任务避免竞争）*/
    lcd_backlight_off();

    /* (3) ST7789 驱动停振荡器和扫描（GRAM 内容保留，省 ~5mA）*/
    lcd_st7789_sleep_in();

    /* (4) 释放所有残留的高算力锁：
     *     万一在 command session 中途（CPU 锁 240 MHz）时系统超时进入睡眠，
     *     必须释放，否则 Light-Sleep 退出后 DFS 无法继续工作。*/
    while (pm_profile_is_high_perf()) {
        pm_profile_high_perf_release();
    }

    /* (5) 配置 GPIO 唤醒源：GPIO0 低电平唤醒（BOOT 键按下 → 拉低）。
     *     ⚠️ 关键：Light-Sleep 必须用 esp_sleep_enable_gpio_wakeup()，
     *        不能用 esp_sleep_enable_ext0_wakeup()——EXT0 是 Deep-Sleep 的 RTC 唤醒源，
     *        在 Light-Sleep 中混用会导致 esp_light_sleep_start() 内部 RTC 配置异常 → 重启。
     *     gpio_wakeup_enable: 配置 GPIO0 为低电平触发唤醒（数字域，Light-Sleep 专用）*/
    esp_err_t err;
    err = gpio_wakeup_enable(PM_LS_WAKE_GPIO, GPIO_INTR_LOW_LEVEL);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "gpio_wakeup_enable(GPIO%d) failed: %s",
                 (int)PM_LS_WAKE_GPIO, esp_err_to_name(err));
    }
    err = esp_sleep_enable_gpio_wakeup();   /* Light-Sleep GPIO 唤醒（非 EXT0）*/
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_sleep_enable_gpio_wakeup failed: %s", esp_err_to_name(err));
    }

    /* 标记即将进入睡眠：undo_button 的 ISR 若此时触发，会知道是唤醒边沿。*/
    s_preparing = true;

    /* 小延迟：让上面的日志 flush，同时给正在路上的 SPI/I2S 操作收尾。*/
    vTaskDelay(pdMS_TO_TICKS(20));

    /* ====== 真正进入 Light-Sleep（CPU 暂停，RAM 保留）======
     * 唤醒原因：GPIO0 低电平 → 从这里的下一行继续执行。*/
    err = esp_light_sleep_start();
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "Light-Sleep exited cleanly");
    } else {
        ESP_LOGW(TAG, "esp_light_sleep_start returned %s (often OK if wake pending)",
                 esp_err_to_name(err));
    }

    /* 打印唤醒原因，方便调试 */
    esp_sleep_wakeup_cause_t cause = esp_sleep_get_wakeup_cause();
    ESP_LOGI(TAG, "wakeup cause: %d (3=GPIO, 5=EXT0, 7=TIMER)", (int)cause);

    /* 清除唤醒配置，避免残留影响正常运行 */
    esp_sleep_disable_wakeup_source(ESP_SLEEP_WAKEUP_GPIO);
    gpio_wakeup_disable(PM_LS_WAKE_GPIO);

    s_preparing = false;

    /* ================================================================
     * Light-Sleep 唤醒恢复序列（与进入相反顺序）
     *   LCD sleep_out → 背光亮 → I2S DMA 恢复 + AFE feed 继续
     * ================================================================ */
    ESP_LOGI(TAG, "=== L6-A wake-up: restore peripherals ===");

    /* (1) ST7789 先 sleep_out（内部等 120ms），让振荡器稳定。*/
    lcd_st7789_sleep_out();

    /* (2) 亮背光 LED */
    lcd_backlight_on();

    /* (3) 重新 enable I2S0 DMA，feed_task 会因为 g_sr_paused=false 自动继续采数据。
     *     speech_resume_i2s 内部会：强制 WAKE_ONLY + 清 MultiNet 状态 + enable_wakenet。*/
    speech_resume_i2s();

    /* L6-A 修复：设置"唤醒宽限期"——接下来 500ms 内的 GPIO0 按键事件一律视为
     * "EXT0 唤醒副作用"，只点亮屏幕不做事/不播语音。*/
    {
        int64_t now = esp_timer_get_time() / 1000;
        s_wake_grace_until_ms = now + PM_LS_WAKE_GRACE_MS;
        ESP_LOGI(TAG, "wake grace window: next %u ms GPIO0 = wake-only (no action)",
                 (unsigned)PM_LS_WAKE_GRACE_MS);
    }

    ESP_LOGI(TAG, "=== L6-A wake-up complete, back to idle (WAKE_ONLY, CPU DFS) ===");
}

/* ================================================================
 * 后台任务：轮询活动时间戳，超时则进入 Light-Sleep
 * ================================================================ */
static void pm_ls_task(void *arg)
{
    (void)arg;
    ESP_LOGI(TAG, "sleep manager started (timeout=%u ms, wake=GPIO%d)",
             (unsigned)PM_LS_TIMEOUT_MS, (int)PM_LS_WAKE_GPIO);

    while (1) {
        ls_lock();
        int64_t last = s_last_act_ms;
        ls_unlock();

        int64_t now     = esp_timer_get_time() / 1000;
        int64_t elapsed = now - last;
        int64_t remain  = (int64_t)PM_LS_TIMEOUT_MS - elapsed;

        if (remain <= 0) {
            /* === 5 分钟到：进入 Light-Sleep === */
            ESP_LOGI(TAG, "idle %lld ms >= %u ms -> entering Light-Sleep",
                     (long long)elapsed, (unsigned)PM_LS_TIMEOUT_MS);

            do_enter_light_sleep();

            /* 睡眠结束：以"唤醒时刻"为新的活动基准，
             * 避免立刻又被判定为"5 分钟没动"再次入睡（用户刚醒，不一定马上说话）。*/
            int64_t wake_t = esp_timer_get_time() / 1000;
            ls_lock();
            s_last_act_ms = wake_t;
            ls_unlock();

            /* 唤醒后给用户 10s 的"反应缓冲"，再开始重新计时 */
            vTaskDelay(pdMS_TO_TICKS(10000));
            continue;
        }

        /* 还没到：睡到"剩余时间"或 500ms 的较小者（避免频繁唤醒）*/
        int64_t sleep_t = (remain < PM_LS_POLL_IDLE_MS) ? remain : PM_LS_POLL_IDLE_MS;
        if (sleep_t < 10) sleep_t = 10;
        vTaskDelay(pdMS_TO_TICKS((uint32_t)sleep_t));
    }
}

/* ================================================================
 * 公共 API
 * ================================================================ */
void pm_sleep_mgr_init(void)
{
    s_lock = xSemaphoreCreateMutex();
    if (s_lock == NULL) {
        ESP_LOGE(TAG, "Failed to create lock");
        return;
    }
    s_last_act_ms = esp_timer_get_time() / 1000;   /* 启动时间算一次活动 */
    s_preparing   = false;

    BaseType_t r = xTaskCreate(pm_ls_task, "pm_ls", PM_LS_TASK_STACK,
                               NULL, PM_LS_TASK_PRIO, &s_task);
    if (r != pdPASS) {
        ESP_LOGE(TAG, "Failed to create pm_ls_task");
    }
}

void pm_sleep_mgr_activity(void)
{
    if (s_lock == NULL) return;
    int64_t now = esp_timer_get_time() / 1000;
    ls_lock();
    s_last_act_ms = now;
    ls_unlock();
    /* L6-A 修复：用户在"唤醒宽限期"内有任何 activity，说明用户想"继续操作"，
     * 立刻清掉宽限期窗口，让下一次按键可以正常工作。
     * 执行顺序：GPIO0 press → lcd_backlight_activity() → pm_sleep_mgr_activity()（这里）
     * → 宽限期清零 → 返回 → undo_button 还在判断 just_wake_ls → 此时宽限期刚清？
     *   不，判断 just_wake_ls 在调用 lcd_backlight_activity() 之前，顺序是：
     *   undo_button press 分支：1. just_wake_ls = is_preparing()  → 2. lcd_backlight_activity()
     *   所以步骤 1 判断时宽限期仍有效 → swallow_pair=true → 步骤 2 才清宽限期，正好。
     *   用户连按第二次时，步骤 1 判断 is_preparing() → 宽限期已清 → 正常撤销。*/
    if (s_wake_grace_until_ms != 0) {
        s_wake_grace_until_ms = 0;
    }
    /* 如果正在 preparing_sleep 中（例如 5min 到了刚 speech_suspend_i2s()，
     * 用户恰好这时按了一下），我们不在这里中断序列——do_enter_light_sleep()
     * 很短，马上就进 Light-Sleep，等唤醒后自然恢复。
     * 真正吞掉"动作/语音"的逻辑在 undo_button.c 的 swallow_pair + release 分支。*/
}

bool pm_sleep_mgr_is_preparing_sleep(void)
{
    if (s_preparing) return true;
    /* L6-A 修复：唤醒后的宽限期内，也视为"准备睡眠" → GPIO0 按键只唤醒不做事 */
    if (s_wake_grace_until_ms != 0) {
        int64_t now = esp_timer_get_time() / 1000;
        if (now < s_wake_grace_until_ms) {
            return true;
        }
        /* 宽限期到，自动清（避免占着判断） */
        s_wake_grace_until_ms = 0;
    }
    return false;
}
