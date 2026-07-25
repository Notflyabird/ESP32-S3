#pragma once

#include <stdint.h>

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

#ifdef __cplusplus
}
#endif
