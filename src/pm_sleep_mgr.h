#pragma once

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief L6-A：电源睡眠管理器（Light-Sleep 安全版，不销毁 AFE 句柄）
 *
 * 作用：长时间（默认 5 分钟）无任何用户活动时，执行节能睡眠序列：
 *   1) speech_suspend_i2s() —— 停 I2S0 DMA + AFE feed（AFE/MultiNet 句柄保留在内存）
 *   2) lcd_backlight_off()  —— 关背光 LED
 *   3) lcd_st7789_sleep_in() —— ST7789 驱动停振荡器/扫描（GRAM 保持）
 *   4) 配置 GPIO0 为 EXT0 唤醒源，调用 esp_light_sleep_start() 进入 Light-Sleep
 *
 * 被 GPIO0 按下唤醒后，反序列恢复：
 *   1) lcd_st7789_sleep_out() + 等 120ms
 *   2) lcd_backlight_on()
 *   3) speech_resume_i2s() —— 重启 I2S0 DMA + 清 g_sr_paused，feed 自动继续
 *
 * 注意：Light-Sleep 期间 CPU 暂停执行，RAM/寄存器内容全部保留；
 *       唤醒后从 esp_light_sleep_start() 的下一条指令继续执行，
 *       FreeRTOS 调度器会恢复，任务无需重建。
 */

/**
 * @brief 初始化睡眠管理器：启动后台超时任务。
 *        需要在 PM / backlight / speech 都初始化完成后调用（app_main 末尾）。
 */
void pm_sleep_mgr_init(void);

/**
 * @brief 通知"有用户活动"：重置 5 分钟计时。
 *        所有用户交互（按键按下、语音唤醒/命令、UI 手动刷新）都应调用此函数。
 *        若当前处于 Light-Sleep 前的准备阶段，会取消睡眠计划。
 */
void pm_sleep_mgr_activity(void);

/**
 * @brief 查询是否处于 Light-Sleep 前的"准备睡眠"阶段。
 *        undo_button 用：若 True，GPIO0 首次按下的唯一作用是唤醒 → 吞掉动作/语音。
 * @return true = 正在进入 / 即将进入 Light-Sleep（GPIO0 按下只唤醒，不做事）
 */
bool pm_sleep_mgr_is_preparing_sleep(void);

#ifdef __cplusplus
}
#endif
