#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ====================== 计分日志持久化（NVS）======================
 * 每一局计分记录掉电保存到 NVS，环形缓冲保留最近 SCORE_LOG_CAPACITY 局。
 * 当前三人分数也一并持久化、开机恢复。
 * 语音"查看计分日志"进入日志页；GPIO0 翻页/退出；语音"清除计分日志"清空全部。
 */

#define SCORE_LOG_CAPACITY   256     /* 环形缓冲容量（局）*/
#define SCORE_LOG_PAGE_ROWS  16      /* 日志页每屏行数 */

typedef enum {
    LOG_OP_ROUND = 0,   /* 普通计分 */
    LOG_OP_RESET = 1,   /* 分数重置（CMD_RESET_SCORE）*/
} log_op_type_t;

/* 单条日志条目 — packed，与 NVS blob 二进制布局严格一致 */
typedef struct __attribute__((packed)) {
    uint16_t round_no;          /* 局号 1..65535（满后回绕至 1）*/
    uint8_t  op;                /* log_op_type_t */
    uint8_t  player;            /* 1..3（重置时 0）*/
    uint8_t  landlord;          /* 1..3（重置时 0）*/
    uint8_t  landlord_win;      /* 0/1 */
    uint8_t  points;            /* 2..20（重置时 0）*/
    int16_t  scores_after[3];   /* 本局结算后总分快照 */
} score_log_entry_t;            /* 14 字节 */

/**
 * @brief 初始化：打开 NVS，读取当前分数与日志 blob，恢复分数到 scorekeeper。
 *        必须在 LCD 初始化之前、scorekeeper_register_commands 之前调用。
 * @return true NVS 打开成功；false NVS 打开失败（降级为仅 RAM，不阻塞启动）。
 */
bool score_log_init(void);

/**
 * @brief 追加一条计分记录并持久化。
 * @note 调用方必须已持有 scorekeeper 的 s_score_lock（写日志路径不复用 log_lock 与之嵌套）。
 */
void score_log_append_round(uint16_t rnd, uint8_t player, uint8_t landlord,
                            bool win, uint8_t points, int s1, int s2, int s3);

/**
 * @brief 追加一条重置记录并持久化。
 * @note 调用方必须已持有 scorekeeper 的 s_score_lock。
 */
void score_log_append_reset(uint16_t rnd, int s1, int s2, int s3);

/**
 * @brief 仅同步当前分数到 NVS（撤销后调用，不写日志条目）。
 * @note 调用方必须已持有 scorekeeper 的 s_score_lock。
 */
void score_log_persist_scores_locked(int s1, int s2, int s3, int landlord);

/**
 * @brief 清空全部历史日志 + 当前分数归零（CMD_CLEAR_LOG）。
 *        清 RAM 缓冲、擦 NVS log key、写 scores=0。
 */
void score_log_clear_all(void);

/**
 * @brief 原子递增并返回下一条局号（1..65535 回绕至 1）。
 */
uint16_t score_log_next_round_no(void);

/* ====================== 日志页视图（仅 RAM）====================== */
bool score_log_view_is_active(void);
void score_log_view_enter(void);          /* 进入日志页（绘制第 1 页）*/
void score_log_view_next_page(void);      /* 短按：下一页回绕至第 1 页 */
void score_log_view_exit(void);           /* 长按/超时退出，重绘主页（幂等）*/

#ifdef __cplusplus
}
#endif
