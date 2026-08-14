#pragma once

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 背光管理：开关型节能控制
 *
 * GPIO7 接 ST7789 背光（LCD_PIN_BL）。默认初始化后背光打开；
 * 用户活动（按键/语音/UI 刷新）调用 lcd_backlight_activity() 重置计时；
 * 超过 LCD_BL_TIMEOUT_MS 无活动则关闭背光，下一次 activity() 重新点亮。
 */

/**
 * @brief 初始化背光 GPIO 并启动后台超时任务。
 *        初始化完成后背光立即打开。
 * @param bl_gpio  背光引脚号（如 LCD_PIN_BL = GPIO_NUM_7）。
 * @return true    成功。
 */
bool lcd_backlight_init(int bl_gpio);

/** @brief 强制打开背光并重置超时计时。*/
void lcd_backlight_on(void);

/** @brief 强制关闭背光（低功耗模式下调用）。*/
void lcd_backlight_off(void);

/**
 * @brief 用户"活动"：重置超时计时，若背光已关则重新点亮。
 *        应由按键/语音/UI 更新等用户交互事件调用。
 */
void lcd_backlight_activity(void);

/** @brief 查询当前背光状态（true = 亮，false = 灭）。*/
bool lcd_backlight_is_on(void);

#ifdef __cplusplus
}
#endif
