#include "pm_sleep_mgr.h"

#include "app_config.h"
#include "driver/gpio.h"
#include "driver/rtc_io.h"
#include "esp_log.h"
#include "esp_pm.h"
#include "esp_sleep.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "lcd_backlight.h"
#include "lcd_st7789.h"
#include "lcd_ui.h"
#include "pm_profile.h"
#include "scorekeeper.h"
#include "speech_recognition.h"

static const char *TAG = "PM_LS";

/* ---------- 配置 ---------- */
/* ⚠️ 休眠超时时间由 app_config.h 统一控制（PM_LS_TIMEOUT_MS），
 * 这里不重复定义，避免两处不一致造成困惑。 */

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
 * 进入 Deep-Sleep 前的准备序列
 *   顺序：SR I2S 停 → 背光关 → LCD sleep_in → 释放残留 PM 锁 → 配 EXT0 唤醒
 * ⚠️ 深度睡眠唤醒 = 整机重启（app_main 重新跑），
 *    esp_deep_sleep_start() 不会返回，唤醒恢复由启动流程完成。
 *   因此这里不需要、也不会有"唤醒恢复序列"（sleep_out/背光/I2S 等）。
 * ================================================================ */
static void do_enter_deep_sleep(void)
{
    ESP_LOGI(TAG, "=== enter Deep-Sleep (wake=GPIO%d, equivalent to reboot) ===",
             (int)PM_LS_WAKE_GPIO);

    /* (1) 停 I2S 采样 + AFE feed。*/
    speech_suspend_i2s();

    /* (2) 关背光 LED（由 lcd_backlight_off 直接写 GPIO，不走超时任务避免竞争）*/
    lcd_backlight_off();
    lcd_backlight_sleep_hold();

    /* (3) ST7789 驱动停振荡器和扫描（GRAM 内容保留，省 ~5mA）*/
    lcd_st7789_sleep_in();

    /* (4) 释放所有残留的高算力锁：DFS 锁不释放会导致深度睡眠失败 */
    while (pm_profile_is_high_perf()) {
        pm_profile_high_perf_release();
    }

    /* (5) ⚠️ 修复"一睡就醒/主动重启"：GPIO0 既是撤销按键又做 EXT0 低电平唤醒。
     *     深度睡眠时数字域断电，普通 gpio_pullup（数字上拉）会失效，
     *     若按键电路无外部上拉，GPIO0 会浮空或被外部下拉成低电平 →
     *     EXT0 低电平唤醒立即触发 → 一睡就醒 → 表现为"主动重启"。
     *     因此必须用 RTC 域上拉（rtc_gpio_pullup_en），它在深度睡眠时仍保持。
     *
     *     步骤：
     *       a) 用 RTC GPIO 上拉把 GPIO0 拉高，并读一次确认已释放（高）
     *       b) 若仍为低 → 放弃本次睡眠（返回），等任务下次再尝试
     *       c) 确认高后，再配置 EXT0 唤醒 */
    /* 先把 GPIO0 切到 RTC 域并启用 RTC 上拉（深度睡眠期间保持）*/
    rtc_gpio_deinit(PM_LS_WAKE_GPIO);
    gpio_config_t gpio0_conf = {
        .pin_bit_mask = (1ULL << PM_LS_WAKE_GPIO),
        .mode         = GPIO_MODE_INPUT,
        .pull_up_en   = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_DISABLE,
    };
    gpio_config(&gpio0_conf);
    vTaskDelay(pdMS_TO_TICKS(10));   /* 等上拉稳定 */

    /* 用 RTC 域上拉覆盖数字上拉，确保深度睡眠期间 GPIO0 保持高电平 */
    gpio_hold_dis(PM_LS_WAKE_GPIO);
    rtc_gpio_pullup_en(PM_LS_WAKE_GPIO);
    rtc_gpio_pulldown_dis(PM_LS_WAKE_GPIO);
    vTaskDelay(pdMS_TO_TICKS(10));

    if (gpio_get_level(PM_LS_WAKE_GPIO) == 0) {
        /* 按键仍被按住 → 不进入睡眠，避免一睡即被自己唤醒 */
        ESP_LOGW(TAG, "GPIO%d still LOW before deep-sleep — skip sleep", (int)PM_LS_WAKE_GPIO);
        rtc_gpio_pullup_dis(PM_LS_WAKE_GPIO);   /* 解除 RTC 上拉，恢复正常 GPIO */
        s_last_act_ms = esp_timer_get_time() / 1000;   /* 重置活动时间，稍后再试 */
        return;
    }

    /* (6) 配置 RTC 唤醒源：GPIO0 低电平唤醒（此时已确认 GPIO0 为高） */
    esp_err_t err;
    err = esp_sleep_enable_ext0_wakeup(PM_LS_WAKE_GPIO, 0);   /* GPIO0 低电平唤醒 */
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_sleep_enable_ext0_wakeup failed: %s", esp_err_to_name(err));
    }

    /* 小延迟：让日志 flush，SPI/I2S 操作收尾 */
    vTaskDelay(pdMS_TO_TICKS(20));

    /* ====== 真正进入 Deep-Sleep（CPU/RAM 断电，唤醒后整机重启）======
     * 此函数不返回。唤醒 → ROM 引导 → bootloader → app_main 重新初始化一切。 */
    ESP_LOGI(TAG, "=== calling esp_deep_sleep_start() — no return ===");
    esp_deep_sleep_start();

    /* 理论上到不了这里 */
    while (1) { vTaskDelay(pdMS_TO_TICKS(1000)); }
}

/* ================================================================
 * 后台任务：轮询活动时间戳，超时则进入 Deep-Sleep
 * ================================================================ */
static void pm_ls_task(void *arg)
{
    (void)arg;
    ESP_LOGI(TAG, "deep-sleep manager started (timeout=%u ms, wake=GPIO%d)",
             (unsigned)PM_LS_TIMEOUT_MS, (int)PM_LS_WAKE_GPIO);

    while (1) {
        ls_lock();
        int64_t last = s_last_act_ms;
        ls_unlock();

        int64_t now     = esp_timer_get_time() / 1000;
        int64_t elapsed = now - last;
        int64_t remain  = (int64_t)PM_LS_TIMEOUT_MS - elapsed;

        if (remain <= 0) {
            /* === 超时：进入 Deep-Sleep（唤醒 = 重启）=== */
            ESP_LOGI(TAG, "idle %lld ms >= %u ms -> entering Deep-Sleep",
                     (long long)elapsed, (unsigned)PM_LS_TIMEOUT_MS);

            do_enter_deep_sleep();   /* 不返回，唤醒后 app_main 重新跑 */

            /* 理论到不了这里 */
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
