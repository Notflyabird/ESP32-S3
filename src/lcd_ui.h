#pragma once

#include <stdint.h>

#include "score_log.h"   /* score_log_entry_t */

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Draw the static frame once at power‑up:
 *        title, horizontal rules, player/status/total labels.
 */
void lcd_ui_init_page(void);

/**
 * @brief Refresh scores and status (partial update — only changed areas).
 *
 * @param landlord_no  1‑based landlord player number (1, 2, or 3).
 * @param s1,s2,s3     Current scores for players 1‑3.
 * @param status       Status string (e.g. "等待语音指令").
 */
void lcd_ui_update(uint8_t landlord_no,
                   int s1, int s2, int s3,
                   const char *status);

/**
 * @brief 绘制计分日志查看页（全屏重绘）。
 *
 * @param page_idx    当前页（0 起）
 * @param num_pages   总页数
 * @param entries     本页条目数组（最新在顶）
 * @param count       本页条目数（0..16）
 */
void lcd_ui_log_draw_page(uint8_t page_idx, uint8_t num_pages,
                          const score_log_entry_t *entries, uint8_t count);

#ifdef __cplusplus
}
#endif
