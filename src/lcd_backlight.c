#include "lcd_backlight.h"

#include "app_config.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "lcd_st7789.h"   /* L5: 背光联动 ST7789 sleep_in/out */
#include "pm_sleep_mgr.h"  /* L6-A: 背光 activity 同步踢 Light-Sleep 计时器 */

static const char *TAG = "LCD_BL";

#define BL_TASK_STACK    8192   /* 调 lcd_st7789_sleep_in/out → SPI transmit 占栈多，需 ≥8KB */
#define BL_TASK_PRIO     2
#define BL_POLL_IDLE_MS  500    /* 背光已灭时的轮询周期（省电）*/

static int               s_bl_gpio = -1;
static volatile bool     s_bl_on   = false;
static volatile int64_t  s_last_activity_ms = 0;
static SemaphoreHandle_t s_lock    = NULL;
static TaskHandle_t      s_task    = NULL;
/* L5: 背光灭后不立刻 sleep_in，给用户 5s 的"晃一下又继续用"缓冲，
 * 避免 20s 一到刚 sleep_in(10ms) 下一秒 activity() 又要 sleep_out(120ms)，
 * 用户体感"按一下 0.1 秒后才亮"。只有连续 5s 都没活动，才进 ST7789 sleep。*/
#define BL_SLEEP_IN_DELAY_MS  5000

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

/* ---------- L5: 背光联动 ST7789 Sleep（任务上下文调用，因为要 vDelay）---------- */
static void do_sleep_in_sequence(void)
{
    /* 先已通过 hw_set(false) 关了背光 LED，接下来让 ST7789 自己的驱动器也停掉
     * （GRAM 保持内容，下一次 sleep_out 后立刻恢复显示，不用重画）。*/
    lcd_st7789_sleep_in();
}

static void do_sleep_out_sequence(void)
{
    /* 亮背光之前必须先 sleep_out（ST7789 数据表 t(SLPOUT) ≥ 120ms）。
     * sleep_out() 内部已经做了 vTaskDelay(125)，返回后 LCD 振荡器已稳定，
     * 再 hw_set(true) 亮背光，画面不会花。*/
    lcd_st7789_sleep_out();
}

/* ---------- 后台任务 ---------- */
static void bl_task(void *arg)
{
    (void)arg;
    ESP_LOGI(TAG, "backlight task started (timeout=%u ms, sleep_in_delay=%u ms)",
             (unsigned)LCD_BL_TIMEOUT_MS, (unsigned)BL_SLEEP_IN_DELAY_MS);

    /* bl_on=true 阶段：
     *   state 0: 正常工作，elapsed < LCD_BL_TIMEOUT_MS → 等
     *   state 0 → elapsed == LCD_BL_TIMEOUT_MS → 关背光（hw_set false + s_bl_on false
     *           + s_bl_off_since_ms = now）→ 进入 BL_SLEEP_IN_DELAY_MS 缓冲
     *           期间如有 activity：直接取消 sleep_in 计划（s_bl_off_since_ms = 0）
     * BL_SLEEP_IN_DELAY_MS 到了且仍 s_bl_on=false：调 do_sleep_in_sequence()（关 LCD 驱动刷新）
     * 此后真正进入低功耗，s_lcd_sleeping=true。activity() 到了时，先 sleep_out 再亮背光。
     */
    static int64_t s_bl_off_since_ms = 0;  /* 0 = 不在缓冲期 */
    static bool    s_lcd_sleeping   = false;

    while (1) {
        bl_lock();
        int64_t last = s_last_activity_ms;
        bool    cur_on = s_bl_on;
        bl_unlock();

        int64_t now = esp_timer_get_time() / 1000;
        int64_t elapsed = now - last;
        int64_t sleep_ms;

        if (cur_on) {
            /* 背光亮：正常模式 */
            s_bl_off_since_ms = 0;  /* 背光开的时候不跟踪缓冲期 */

            if (elapsed >= (int64_t)LCD_BL_TIMEOUT_MS) {
                /* 超时：只关背光 LED，LCD 驱动刷新先不关（先给 5s 缓冲）*/
                ESP_LOGI(TAG, "idle %lld ms -> backlight LED OFF (LCD driver will sleep in +%u ms)",
                         (long long)elapsed, (unsigned)BL_SLEEP_IN_DELAY_MS);
                bl_lock();
                hw_set(false);
                s_bl_on = false;
                bl_unlock();
                s_bl_off_since_ms = now;
                s_lcd_sleeping   = false;
                sleep_ms         = BL_SLEEP_IN_DELAY_MS;  /* 等 5s 缓冲期再决定是否 LCD sleep_in */
            } else {
                sleep_ms = (int64_t)LCD_BL_TIMEOUT_MS - elapsed;
                if (sleep_ms < 10) sleep_ms = 10;
            }
        } else {
            /* 背光已灭 */
            if (!s_lcd_sleeping && s_bl_off_since_ms != 0) {
                int64_t off_elapsed = now - s_bl_off_since_ms;
                if (off_elapsed >= (int64_t)BL_SLEEP_IN_DELAY_MS) {
                    /* 缓冲期到，仍没活动 → 正式调 LCD sleep_in 停驱动刷新（省 ~5mA）*/
                    ESP_LOGI(TAG, "idle %lld ms after LED off -> ST7789 SLEEP_IN", (long long)off_elapsed);
                    do_sleep_in_sequence();
                    s_lcd_sleeping   = true;
                    s_bl_off_since_ms = 0;  /* 缓冲期结束 */
                } else {
                    /* 缓冲期内：等到 BL_SLEEP_IN_DELAY_MS 剩余时间 */
                    int64_t rem = (int64_t)BL_SLEEP_IN_DELAY_MS - off_elapsed;
                    sleep_ms = (rem < BL_POLL_IDLE_MS) ? rem : BL_POLL_IDLE_MS;
                    if (sleep_ms < 10) sleep_ms = 10;
                    vTaskDelay(pdMS_TO_TICKS((uint32_t)sleep_ms));
                    continue;
                }
            }
            sleep_ms = BL_POLL_IDLE_MS;  /* 已 LCD sleeping：低频轮询 */
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

    /* ⚠️ 深度睡眠唤醒 = 整机重启，会重新跑本函数。
     * 若上次睡眠时用了 gpio_hold_en / gpio_deep_sleep_hold_en，唤醒后该引脚
     * 仍被硬件锁住（低电平），导致下面 gpio_config / gpio_set_level 不生效
     * → 背光永远点不亮。所以初始化第一步必须先解除 GPIO hold。*/
    gpio_hold_dis((gpio_num_t)bl_gpio);

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
    /* L5：如果 LCD 驱动已经 sleep_in，必须先 sleep_out + 等 120ms，再亮背光。
     * sleep_out() 会检查 if (!s_sleeping) 直接 return，所以即便已经亮也安全，不卡。*/
    if (lcd_st7789_is_sleeping()) {
        do_sleep_out_sequence();
    }
    bl_lock();
    if (!s_bl_on) {
        hw_set(true);
        s_bl_on = true;
        ESP_LOGD(TAG, "backlight ON (forced)");
    }
    s_last_activity_ms = now;
    bl_unlock();
    /* L6-A："强制开背光"也算活动，重置 5 分钟 Light-Sleep 计时器。*/
    pm_sleep_mgr_activity();
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

/* ---------- Light-Sleep 期间锁住背光电平 ---------- */
void lcd_backlight_sleep_hold(void)
{
    if (s_bl_gpio < 0) return;
    bl_lock();
    hw_set(false);            /* 确保是低电平（灭）*/
    s_bl_on = false;
    bl_unlock();

    /* 锁住 GPIO 状态：Light-Sleep 时 ESP32-S3 会复位 GPIO 为默认电平
     * （CONFIG_ESP_SLEEP_GPIO_RESET_WORKAROUND），若不 hold，背光引脚会
     * 被拉回默认 → 睡眠期间背光亮起。 */
    gpio_hold_en((gpio_num_t)s_bl_gpio);
    gpio_deep_sleep_hold_en();   /* 让 hold 在 Light/Deep-Sleep 期间都生效 */
    ESP_LOGD(TAG, "backlight GPIO%d held LOW for sleep", s_bl_gpio);
}

void lcd_backlight_wake_release(void)
{
    if (s_bl_gpio < 0) return;
    /* 先解除 hold，恢复 GPIO 正常控制 */
    gpio_hold_dis((gpio_num_t)s_bl_gpio);
    /* 重新配置为输出，确保可继续驱动 */
    gpio_config_t bl_conf = {
        .pin_bit_mask = (1ULL << s_bl_gpio),
        .mode         = GPIO_MODE_OUTPUT,
        .pull_up_en   = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_DISABLE,
    };
    gpio_config(&bl_conf);
    ESP_LOGD(TAG, "backlight GPIO%d hold released", s_bl_gpio);
}

void lcd_backlight_activity(void)
{
    if (s_lock == NULL) return;
    int64_t now = esp_timer_get_time() / 1000;
    /* L5：activity 要"先唤醒 LCD sleep_out + 120ms 等待"再亮背光。
     * 如果 ST7789 还没 sleep_in，sleep_out() 会立即返回，0 耗时。*/
    bool need_sleep_out = lcd_st7789_is_sleeping();
    if (need_sleep_out) {
        do_sleep_out_sequence();
    }
    bl_lock();
    s_last_activity_ms = now;
    if (!s_bl_on) {
        /* 如果不是 need_sleep_out（只是 5s 缓冲期内），sleep_out 已 return，直接亮背光即可；
         * 如果真 sleep_out 过，120ms 也已等完，此时亮背光不会花屏。*/
        hw_set(true);
        s_bl_on = true;
        ESP_LOGD(TAG, "backlight ON (activity)");
    }
    bl_unlock();
    /* L6-A："用户活动"同时踢 Light-Sleep 5 分钟计时器，
     * 所有交互点（语音唤醒/命令、按键按下、UI 刷新）都会走这里，
     * 所以在这里加一处就等于所有交互点都通知到了。*/
    pm_sleep_mgr_activity();
}

bool lcd_backlight_is_on(void)
{
    if (s_lock == NULL) return false;
    bl_lock();
    bool on = s_bl_on;
    bl_unlock();
    return on;
}
