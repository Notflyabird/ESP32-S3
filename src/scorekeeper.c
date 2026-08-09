#include "scorekeeper.h"

#include <stdio.h>
#include <string.h>

#include "esp_err.h"
#include "esp_log.h"
#include "esp_mn_speech_commands.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "lcd_ui.h"
#include "score_log.h"
#include "voice_player.h"

static const char *TAG = "DDZ_SCORE";

#define CMD_QUERY_SCORE 1
#define CMD_RESET_SCORE 2
#define CMD_VIEW_LOG    3
#define CMD_CLEAR_LOG   4
#define CMD_SCORE_BASE 100

/* 撤销窗口：10 秒内有效 */
#define UNDO_WINDOW_MS 10000

typedef struct {
    const char *spoken;
    int points;
} point_phrase_t;

static int s_score[3] = {0, 0, 0};
static int s_landlord = 0;

/* 最近一次操作快照（用于撤销） */
typedef struct {
    bool       valid;            /* 是否存在可撤销操作 */
    int        scores_before[3]; /* 操作前分数快照 */
    int        landlord_before;  /* 操作前地主号 */
    int        landlord_after;   /* 操作后地主号 */
    bool       was_reset;        /* 是否是重置操作 */
    int64_t    timestamp_ms;     /* 操作完成的时间戳 */
    /* 被撤销的操作（结构化，供语音反馈重建句子）*/
    int        op_player;        /* 计分玩家 1-3，重置时为 0 */
    bool       op_landlord_win;  /* true=地主赢，false=地主输 */
    int        op_points;        /* 计分分值 */
} undo_snapshot_t;

static undo_snapshot_t s_last_op;
static SemaphoreHandle_t s_score_lock;

static const char *const PLAYER_PHRASES[3] = {
    "yi hao",
    "er hao",
    "san hao",
};

static const point_phrase_t POINT_PHRASES[] = {
    {"liang fen", 2},
    {"si fen", 4},
    {"liu fen", 6},
    {"ba fen", 8},
    {"yi shi fen", 10},
    {"yi shi er fen", 12},
    {"yi shi si fen", 14},
    {"yi shi liu fen", 16},
    {"yi shi ba fen", 18},
    {"er shi fen", 20},
};

static int score_total(void)
{
    return s_score[0] + s_score[1] + s_score[2];
}

void scorekeeper_print_scores(const char *title)
{
    ESP_LOGI(TAG, "%s: P1=%d, P2=%d, P3=%d, total=%d",
             title, s_score[0], s_score[1], s_score[2], score_total());
}

void scorekeeper_get_scores(int *s1, int *s2, int *s3, int *landlord)
{
    *s1 = s_score[0];
    *s2 = s_score[1];
    *s3 = s_score[2];
    if (landlord) {
        *landlord = s_landlord;
    }
}

void scorekeeper_restore_state(int scores[3], int landlord)
{
    if (s_score_lock == NULL) {
        s_score_lock = xSemaphoreCreateMutex();
    }
    xSemaphoreTake(s_score_lock, portMAX_DELAY);
    s_score[0] = scores[0];
    s_score[1] = scores[1];
    s_score[2] = scores[2];
    s_landlord = landlord;
    /* 跨重启不可撤销：避免开机后误按 GPIO0 撤销恢复的分数 */
    s_last_op.valid = false;
    xSemaphoreGive(s_score_lock);
    scorekeeper_print_scores("Restored");
}

static void reset_scores(void)
{
    s_score[0] = 0;
    s_score[1] = 0;
    s_score[2] = 0;
    s_landlord = 0;
    ESP_LOGI(TAG, "All scores reset");
    scorekeeper_print_scores("After reset");
}

static void undo_snapshot_begin(int64_t *save_scores, int *save_landlord)
{
    if (s_score_lock == NULL) {
        s_score_lock = xSemaphoreCreateMutex();
    }
    xSemaphoreTake(s_score_lock, portMAX_DELAY);
    save_scores[0] = s_score[0];
    save_scores[1] = s_score[1];
    save_scores[2] = s_score[2];
    *save_landlord = s_landlord;
}

static void undo_snapshot_commit(const int64_t *scores_before, int landlord_before,
                                 bool was_reset, int op_player,
                                 bool op_landlord_win, int op_points)
{
    s_last_op.valid = true;
    s_last_op.scores_before[0] = (int)scores_before[0];
    s_last_op.scores_before[1] = (int)scores_before[1];
    s_last_op.scores_before[2] = (int)scores_before[2];
    s_last_op.landlord_before = landlord_before;
    s_last_op.landlord_after  = s_landlord;
    s_last_op.was_reset       = was_reset;
    s_last_op.timestamp_ms    = esp_timer_get_time() / 1000;
    s_last_op.op_player       = op_player;
    s_last_op.op_landlord_win = op_landlord_win;
    s_last_op.op_points       = op_points;

    /* 追加持久化日志 + 同步当前分数（仍持 s_score_lock；score_log 内部取 s_log_lock）*/
    uint16_t rnd = score_log_next_round_no();
    if (was_reset) {
        score_log_append_reset(rnd, s_score[0], s_score[1], s_score[2]);
    } else {
        score_log_append_round(rnd, (uint8_t)op_player, (uint8_t)s_landlord,
                               op_landlord_win, (uint8_t)op_points,
                               s_score[0], s_score[1], s_score[2]);
    }
    score_log_persist_scores_locked(s_score[0], s_score[1], s_score[2], s_landlord);

    xSemaphoreGive(s_score_lock);
}

static void undo_snapshot_abort(void)
{
    xSemaphoreGive(s_score_lock);
}

static int make_score_command_id(int player, bool landlord_win, int points)
{
    const int player_index = player - 1;
    const int outcome_index = landlord_win ? 0 : 1;
    const int point_index = points / 2 - 1;
    return CMD_SCORE_BASE + player_index * 20 + outcome_index * 10 + point_index;
}

static bool parse_score_command_id(int command, int *player, bool *landlord_win, int *points)
{
    int value = command - CMD_SCORE_BASE;
    if (value < 0 || value >= 60) {
        return false;
    }

    const int player_index = value / 20;
    value %= 20;
    const int outcome_index = value / 10;
    const int point_index = value % 10;

    *player = player_index + 1;
    *landlord_win = (outcome_index == 0);
    *points = (point_index + 1) * 2;
    return true;
}

static void settle_round(int landlord, bool landlord_win, int points)
{
    const int landlord_delta = landlord_win ? points : -points;
    const int farmer_delta = landlord_win ? -(points / 2) : (points / 2);
    const int landlord_index = landlord - 1;

    s_landlord = landlord;

    ESP_LOGI(TAG, "P%d landlord %s %d pts", landlord, landlord_win ? "wins" : "loses", points);

    for (int i = 0; i < 3; ++i) {
        s_score[i] += (i == landlord_index) ? landlord_delta : farmer_delta;
    }

    ESP_LOGI(TAG, "Scores: P1=%d, P2=%d, P3=%d", s_score[0], s_score[1], s_score[2]);

    if (score_total() != 0) {
        ESP_LOGE(TAG, "Total check failed");
    }
}

static void speak_score_update(int player, bool landlord_win, int points)
{
    voice_speak_score_update((uint8_t)player, landlord_win, points);
}

static void speak_query_score(void)
{
    voice_speak_query(s_score[0], s_score[1], s_score[2]);
}

static void speak_reset(void)
{
    voice_speak_reset();
}

void scorekeeper_apply_command(int command)
{
    /* 日志页期间收到任何非"查看日志"命令：先退出日志页回主页再处理 */
    if (command != CMD_VIEW_LOG && score_log_view_is_active()) {
        score_log_view_exit();
    }

    int player = 0;
    int points = 0;
    bool landlord_win = false;

    if (parse_score_command_id(command, &player, &landlord_win, &points)) {
        int64_t before[3];
        int before_landlord;
        undo_snapshot_begin(before, &before_landlord);

        settle_round(player, landlord_win, points);

        undo_snapshot_commit(before, before_landlord, false,
                             player, landlord_win, points);

        speak_score_update(player, landlord_win, points);
        lcd_ui_update((uint8_t)player, s_score[0], s_score[1], s_score[2], "计分已更新");
        return;
    }

    switch (command) {
    case CMD_QUERY_SCORE:
        scorekeeper_print_scores("Query");
        speak_query_score();
        lcd_ui_update((uint8_t)s_landlord, s_score[0], s_score[1], s_score[2], "查询中...");
        break;
    case CMD_RESET_SCORE: {
        int64_t before[3];
        int before_landlord;
        undo_snapshot_begin(before, &before_landlord);

        reset_scores();
        undo_snapshot_commit(before, before_landlord, true, 0, false, 0);

        speak_reset();
        lcd_ui_update(0, s_score[0], s_score[1], s_score[2], "重置完成");
        break;
    }
    case CMD_VIEW_LOG:
        /* 进入日志页（若已在则重置到第 1 页重绘）*/
        score_log_view_enter();
        voice_speak_view_log();
        break;
    case CMD_CLEAR_LOG: {
        /* 清空全部历史日志 + 当前分数归零（全新开始）*/
        int64_t before[3];
        int before_landlord;
        undo_snapshot_begin(before, &before_landlord);
        reset_scores();
        score_log_clear_all();
        s_last_op.valid = false;
        undo_snapshot_abort();
        lcd_ui_update(0, 0, 0, 0, "全部已清空");
        voice_speak_clear_log();
        break;
    }
    default:
        ESP_LOGW(TAG, "Unknown command id: %d", command);
        break;
    }
}

bool scorekeeper_undo_last(void)
{
    if (s_score_lock == NULL) {
        s_score_lock = xSemaphoreCreateMutex();
    }
    xSemaphoreTake(s_score_lock, portMAX_DELAY);

    bool ok = false;
    if (!s_last_op.valid) {
        ESP_LOGW(TAG, "Undo failed: no previous operation");
        xSemaphoreGive(s_score_lock);
        voice_speak_undo_none();
        return false;
    }

    int64_t now_ms = esp_timer_get_time() / 1000;
    int64_t elapsed = now_ms - s_last_op.timestamp_ms;
    if (elapsed > UNDO_WINDOW_MS) {
        ESP_LOGW(TAG, "Undo failed: window closed (%lld ms ago)", (long long)elapsed);
        s_last_op.valid = false;
        xSemaphoreGive(s_score_lock);
        voice_speak_undo_timeout();
        return false;
    }

    /* 回滚 */
    s_score[0]   = s_last_op.scores_before[0];
    s_score[1]   = s_last_op.scores_before[1];
    s_score[2]   = s_last_op.scores_before[2];
    s_landlord   = s_last_op.landlord_before;
    s_last_op.valid = false;
    ok = true;

    /* 释放锁前拷贝结构化字段，供锁外语音反馈使用 */
    bool was_reset       = s_last_op.was_reset;
    int  op_player       = s_last_op.op_player;
    bool op_landlord_win = s_last_op.op_landlord_win;
    int  op_points       = s_last_op.op_points;
    int  s1 = s_score[0], s2 = s_score[1], s3 = s_score[2];
    uint8_t landlord_disp = (uint8_t)(s_landlord > 0 ? s_landlord : 0);

    /* 同步撤销后分数到 NVS（撤销不进日志条目，仅同步当前分数）*/
    score_log_persist_scores_locked(s1, s2, s3, s_landlord);

    xSemaphoreGive(s_score_lock);

    ESP_LOGI(TAG, "Undo success: rolled back op(player=%d,win=%d,pts=%d,reset=%d) (elapsed=%lldms)",
             op_player, op_landlord_win, op_points, was_reset, (long long)elapsed);
    scorekeeper_print_scores("After undo");

    lcd_ui_update(landlord_disp, s1, s2, s3, "已撤销");
    voice_speak_undo_result(was_reset, (uint8_t)op_player, op_landlord_win,
                            op_points, s1, s2, s3);

    return ok;
}

static bool add_command_checked(esp_err_t err, int command_id, const char *phrase)
{
    if (err == ESP_OK) {
        return true;
    }
    ESP_LOGE(TAG, "Failed to add command [%s]: %s", phrase, esp_err_to_name(err));
    return false;
}

static bool add_score_command(int player, bool landlord_win, int points, const char *point_phrase)
{
    char command_phrase[64];
    const int command_id = make_score_command_id(player, landlord_win, points);

    snprintf(command_phrase, sizeof(command_phrase), "%s di zhu %s %s",
             PLAYER_PHRASES[player - 1], landlord_win ? "ying" : "shu", point_phrase);

    return add_command_checked(esp_mn_commands_add(command_id, command_phrase),
                               command_id, command_phrase);
}

bool scorekeeper_register_commands(void)
{
    bool ok = true;

    for (int player = 1; player <= 3 && ok; ++player) {
        for (size_t i = 0; i < sizeof(POINT_PHRASES) / sizeof(POINT_PHRASES[0]) && ok; ++i) {
            ok = add_score_command(player, true, POINT_PHRASES[i].points, POINT_PHRASES[i].spoken);
            if (ok) {
                ok = add_score_command(player, false, POINT_PHRASES[i].points, POINT_PHRASES[i].spoken);
            }
        }
    }

    return ok &&
           add_command_checked(esp_mn_commands_add(CMD_QUERY_SCORE, "cha xun fen shu"),
                               CMD_QUERY_SCORE, "cha xun fen shu") &&
           add_command_checked(esp_mn_commands_add(CMD_RESET_SCORE, "chong zhi suo you fen shu"),
                               CMD_RESET_SCORE, "chong zhi suo you fen shu") &&
           add_command_checked(esp_mn_commands_add(CMD_VIEW_LOG, "cha kan ji fen ri zhi"),
                               CMD_VIEW_LOG, "cha kan ji fen ri zhi") &&
           add_command_checked(esp_mn_commands_add(CMD_CLEAR_LOG, "qing chu ji fen ri zhi"),
                               CMD_CLEAR_LOG, "qing chu ji fen ri zhi");
}