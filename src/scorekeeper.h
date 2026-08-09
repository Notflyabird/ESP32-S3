#pragma once

#include <stdbool.h>

bool scorekeeper_register_commands(void);
void scorekeeper_apply_command(int command);
void scorekeeper_print_scores(const char *title);
void scorekeeper_get_scores(int *s1, int *s2, int *s3, int *landlord);

/**
 * @brief 开机恢复持久化分数（由 score_log_init 调用）。
 *        强制 s_last_op.valid=false（跨重启不可撤销）。
 */
void scorekeeper_restore_state(int scores[3], int landlord);

/**
 * @brief 尝试撤销最近一次计分操作。
 * @return true 撤销成功；false 没有可撤销的操作或已超过10秒窗口
 */
bool scorekeeper_undo_last(void);

