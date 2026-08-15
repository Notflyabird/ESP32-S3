#pragma once

#include <stdbool.h>

bool speech_recognition_init(void);
bool speech_recognition_start(void);
void speech_recognition_print_pipeline(void);

/**
 * @brief L4：ESP-SR 当前运行模式（仅供调试/状态指示）。
 *        WAKE_ONLY = 默认待机，只跑唤醒词，CPU 默认 80MHz（DFS）。
 *        FULL = 刚被唤醒词触发，或 command session 中，MultiNet 全开，CPU 锁 240 MHz。
 */
typedef enum {
    SR_MODE_WAKE_ONLY = 0,
    SR_MODE_FULL      = 1,
} sr_mode_t;

/** @brief 查询当前运行模式（WAKE_ONLY / FULL）。*/
sr_mode_t speech_get_mode(void);

/**
 * @brief L6-A：挂起 I2S 采样 + AFE feed（不销毁 AFE / 模型，安全版）。
 *        用于进入 Light-Sleep 之前：
 *          1) 设置 g_sr_paused=true，让 feed_task / detect_task 先退到等待
 *          2) 调 audio_input_stop() 停 I2S0 DMA
 *        唤醒后调用 speech_resume_i2s() 反操作。
 */
void speech_suspend_i2s(void);

/** @brief L6-A：Light-Sleep 唤醒后，重新 enable I2S0 DMA + 清 g_sr_paused。*/
void speech_resume_i2s(void);

