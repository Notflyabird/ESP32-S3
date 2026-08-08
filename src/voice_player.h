#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ====================== 语音播放器（预生成素材，替换 esp-tts）======================
 * 非阻塞：speak_* 函数把任务入队后立即返回，由专用 voice_task 在 core 1 上
 * 完成 SR 暂停 → 解除静音 → DSP 重置 → 素材拼接播放 → 排空 → 静音 → 恢复 SR。
 */

/**
 * @brief 初始化语音播放器（创建队列与任务）。
 * @return true 成功；false 队列或任务创建失败。
 */
bool voice_player_init(void);

/**
 * @brief 当前是否有语音正在播放（非阻塞）。
 */
bool voice_is_playing(void);

/**
 * @brief 反初始化，释放队列与任务。
 */
void voice_player_deinit(void);

/* ---------- 非阻塞播报 API ---------- */
void voice_speak_boot(void);                                                /* 开机语 */
void voice_speak_im_here(void);                                             /* 唤醒应答"我在" */
void voice_speak_reset(void);                                               /* 分数已重置 */
void voice_speak_undo_none(void);                                           /* 没有可撤销的计分 */
void voice_speak_undo_timeout(void);                                        /* 撤销时间已超过十秒 */
void voice_speak_score_update(uint8_t player, bool landlord_win, int points);/* X号地主赢/输X分 */
void voice_speak_query(int s1, int s2, int s3);                             /* 当前分数... */
void voice_speak_undo_result(bool was_reset, uint8_t op_player,
                             bool op_landlord_win, int op_points,
                             int s1, int s2, int s3);                       /* 已撤销... */

#ifdef __cplusplus
}
#endif
