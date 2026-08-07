#include "undo_button.h"

#include "driver/gpio.h"
#include "esp_err.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"

#include "scorekeeper.h"

static const char *TAG = "UNDO_BTN";

#define UNDO_GPIO       GPIO_NUM_0
/* 硬件消抖：30ms 内同一按键事件忽略 */
#define DEBOUNCE_MS     30

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
    int64_t last_press_ms = 0;

    ESP_LOGI(TAG, "GPIO%d undo button ready (press within 10s after scoring)", UNDO_GPIO);

    while (xQueueReceive(s_btn_queue, &evt, portMAX_DELAY) == pdTRUE) {
        /* 任务级第二次消抖 & 只在按下下降沿触发（释放不触发） */
        if (!evt.pressed) {
            continue;
        }
        if (evt.timestamp_ms - last_press_ms < DEBOUNCE_MS) {
            continue;
        }
        last_press_ms = evt.timestamp_ms;

        ESP_LOGI(TAG, "Undo button pressed (GPIO%d low)", UNDO_GPIO);
        /* 调用计分模块 undo 逻辑，TTS 友好反馈由 scorekeeper 输出 */
        scorekeeper_undo_last();
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
