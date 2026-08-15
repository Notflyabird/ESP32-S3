#include "lcd_ui.h"

#include <stdbool.h>
#include <stdio.h>

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "lcd_backlight.h"
#include "lcd_st7789.h"

/* ======================== Colour constants ======================== */
#define BLACK   0x0000
#define WHITE   0xFFFF
#define RED     0xF800
#define YELLOW  0xFFE0
#define GREEN   0x07E0
#define GREY    0x8410

/* ======================== Layout geometry ========================= */
#define SCR_W   240

/* Title — "斗地主计分" 5字×16px=80px, (240-80)/2=80 */
#define TITLE_X    80
#define TITLE_Y    8

/* Horizontal rules */
#define RULE1_Y    44
#define RULE2_Y    182

/* Player rows */
#define PL_LABEL_X  12
#define PL_SCORE_X  98
#define PL1_Y       52
#define PL2_Y       96
#define PL3_Y       140

/* Status line — "状态:" = 16+16+8=40px, 12+40+8=60 */
#define ST_LABEL_X  12
#define ST_TEXT_X   60
#define ST_Y        192

/* Total line — "总分:" = 16+16+8=40px, 12+40+8=60 */
#define TT_LABEL_X  12
#define TT_TEXT_X   60
#define TT_Y        232

/* Hint */
#define HINT_X      8
#define HINT_Y      285

/* Score field clearing rectangle */
#define SCORE_BG_W  80
#define SCORE_BG_H  16

/* Label width for erase (e.g. "P1:" = 3 chars = 24 px, "Status:" = 7*8=56) */
#define LABEL_ERASE_W 72

/* ======================== LCD 互斥锁 ============================== */
/* spi_device_polling_transmit 非线程安全；undo_btn(未绑核,prio6) 与
 * sr_detect(core1,prio5) 都会画屏，需互斥。mutex 自带优先级继承。
 * ⚠️ 锁必须在 main.c 初始化阶段一次性创建，不能懒加载：
 *    懒加载的 if(NULL) xSemaphoreCreateMutex() 不是原子的，
 *    两个任务同时第一次调 lcd_ui_update 会创建两个锁、其中一个泄漏
 *    → 两个任务拿到不同的锁 → 并发访问 SPI 崩溃。*/
static SemaphoreHandle_t s_lcd_lock = NULL;

/* 内部用：初始化时（一次）创建互斥锁。调用方：lcd_ui_init_page() */
static inline void lcd_ui_ensure_lock(void)
{
    if (s_lcd_lock == NULL) {
        s_lcd_lock = xSemaphoreCreateMutex();
    }
}

static void lcd_lock(void)
{
    xSemaphoreTake(s_lcd_lock, portMAX_DELAY);
}

static void lcd_unlock(void)
{
    xSemaphoreGive(s_lcd_lock);
}

/* ======================== 日志页布局 ============================= */
#define LOG_TITLE_Y   0
#define LOG_RULE1_Y   16
#define LOG_HEADER_Y  20
#define LOG_RULE2_Y   36
#define LOG_ROW0_Y    40
#define LOG_ROW_H     16
#define LOG_HINT_Y    298

/* ======================== Helpers ================================= */

static uint16_t fmt_score(char *buf, size_t sz, int val)
{
    snprintf(buf, sz, "%d", val);
    return lcd_st7789_string_width(buf);
}

/** "P1:" / "P2:" / "P3:" label + right‑aligned score */
static void draw_player_row(uint16_t y, int player_no, int score, bool is_landlord)
{
    uint16_t fg = is_landlord ? RED : WHITE;
    char buf[16];

    /* Label */
    snprintf(buf, sizeof(buf), "P%d:", player_no);
    lcd_st7789_fill_rect(PL_LABEL_X, y, LABEL_ERASE_W, 16, BLACK);
    lcd_st7789_draw_string(PL_LABEL_X, y, buf, fg, BLACK);

    /* Score */
    uint16_t sw = fmt_score(buf, sizeof(buf), score);
    lcd_st7789_fill_rect(PL_SCORE_X, y, SCORE_BG_W, SCORE_BG_H, BLACK);
    lcd_st7789_draw_string(PL_SCORE_X + SCORE_BG_W - sw, y, buf, fg, BLACK);
}

/* ======================== Public API ============================== */

void lcd_ui_init_page(void)
{
    /* ⚠️ 必须在第一个 lcd_lock() 之前（也就是在任何 lcd_ui_* API 被并发调之前）
     *    把互斥锁创建好。lcd_ui_init_page 在 app_main 初始化阶段串行调用，无并发，安全。*/
    lcd_ui_ensure_lock();
    lcd_lock();
    lcd_st7789_fill_screen(BLACK);

    /* Title */
    lcd_st7789_draw_string(TITLE_X, TITLE_Y, "斗地主计分", YELLOW, BLACK);

    /* Horizontal rules */
    lcd_st7789_fill_rect(0, RULE1_Y, SCR_W, 2, WHITE);
    lcd_st7789_fill_rect(0, RULE2_Y, SCR_W, 2, WHITE);

    /* Static labels (white) */
    for (int i = 1; i <= 3; i++) {
        char buf[8];
        snprintf(buf, sizeof(buf), "P%d:", i);
        lcd_st7789_draw_string(PL_LABEL_X,
                               (i == 1) ? PL1_Y : (i == 2) ? PL2_Y : PL3_Y,
                               buf, WHITE, BLACK);
    }

    lcd_st7789_draw_string(ST_LABEL_X, ST_Y, "状态:", WHITE, BLACK);
    lcd_st7789_draw_string(TT_LABEL_X, TT_Y, "总分:",  WHITE, BLACK);

    /* Hint – grey */
    lcd_st7789_draw_string(HINT_X, HINT_Y, "语音指令", GREY, BLACK);
    lcd_unlock();
}

void lcd_ui_update(uint8_t landlord_no,
                   int s1, int s2, int s3,
                   const char *status)
{
    lcd_backlight_activity();
    lcd_lock();
    char buf[32];

    draw_player_row(PL1_Y, 1, s1, (landlord_no == 1));
    draw_player_row(PL2_Y, 2, s2, (landlord_no == 2));
    draw_player_row(PL3_Y, 3, s3, (landlord_no == 3));

    /* Status */
    if (status) {
        lcd_st7789_fill_rect(ST_TEXT_X, ST_Y, 160, 16, BLACK);
        lcd_st7789_draw_string(ST_TEXT_X, ST_Y, status, WHITE, BLACK);
    }

    /* Total */
    int total = s1 + s2 + s3;
    uint16_t total_color = (total == 0) ? GREEN : RED;
    lcd_st7789_fill_rect(TT_TEXT_X, TT_Y, 100, 16, BLACK);
    uint16_t sw = fmt_score(buf, sizeof(buf), total);
    lcd_st7789_draw_string(TT_TEXT_X + 100 - sw, TT_Y, buf, total_color, BLACK);
    lcd_unlock();
}

/* ======================== 日志查看页 ============================== */

void lcd_ui_log_draw_page(uint8_t page_idx, uint8_t num_pages,
                          const score_log_entry_t *entries, uint8_t count)
{
    lcd_backlight_activity();
    lcd_lock();
    lcd_st7789_fill_screen(BLACK);

    char buf[40];

    /* 标题 "计分日志 Pxx/Pyy"（居中）*/
    snprintf(buf, sizeof(buf), "计分日志 P%02u/P%02u",
             (unsigned)page_idx + 1, (unsigned)num_pages);
    uint16_t tw = lcd_st7789_string_width(buf);
    lcd_st7789_draw_string((SCR_W - tw) / 2, LOG_TITLE_Y, buf, YELLOW, BLACK);

    /* 横线 + 列头 + 横线 */
    lcd_st7789_fill_rect(0, LOG_RULE1_Y, SCR_W, 2, WHITE);
    /* 列头：逐列绘制中文标签以与数据列对齐 */
    lcd_st7789_draw_string(0,   LOG_HEADER_Y, "局", GREY, BLACK);
    lcd_st7789_draw_string(32,  LOG_HEADER_Y, "玩", GREY, BLACK);
    lcd_st7789_draw_string(56,  LOG_HEADER_Y, "地", GREY, BLACK);
    lcd_st7789_draw_string(80,  LOG_HEADER_Y, "分", GREY, BLACK);
    lcd_st7789_draw_string(104, LOG_HEADER_Y, "P1", GREY, BLACK);
    lcd_st7789_draw_string(144, LOG_HEADER_Y, "P2", GREY, BLACK);
    lcd_st7789_draw_string(184, LOG_HEADER_Y, "P3", GREY, BLACK);
    lcd_st7789_fill_rect(0, LOG_RULE2_Y, SCR_W, 2, WHITE);

    if (count == 0) {
        const char *msg = "无记录";
        uint16_t mw = lcd_st7789_string_width(msg);
        lcd_st7789_draw_string((SCR_W - mw) / 2, LOG_ROW0_Y + 48, msg, GREY, BLACK);
    } else {
        for (uint8_t i = 0; i < count && i < SCORE_LOG_PAGE_ROWS; i++) {
            const score_log_entry_t *e = &entries[i];
            uint16_t y = (uint16_t)(LOG_ROW0_Y + i * LOG_ROW_H);
            if (e->op == LOG_OP_RESET) {
                /* 重置行：局号 + "重置" + 三人分数 */
                snprintf(buf, sizeof(buf), "%3u", (unsigned)e->round_no);
                lcd_st7789_draw_string(0, y, buf, GREY, BLACK);
                lcd_st7789_draw_string(32, y, "重置", GREY, BLACK);
                snprintf(buf, sizeof(buf), "%4d", e->scores_after[0]);
                lcd_st7789_draw_string(104, y, buf, GREY, BLACK);
                snprintf(buf, sizeof(buf), "%4d", e->scores_after[1]);
                lcd_st7789_draw_string(144, y, buf, GREY, BLACK);
                snprintf(buf, sizeof(buf), "%4d", e->scores_after[2]);
                lcd_st7789_draw_string(184, y, buf, GREY, BLACK);
            } else {
                /* 普通计分行：逐列绘制 */
                snprintf(buf, sizeof(buf), "%3u", (unsigned)e->round_no);
                lcd_st7789_draw_string(0, y, buf, WHITE, BLACK);
                snprintf(buf, sizeof(buf), "%u", (unsigned)e->player);
                lcd_st7789_draw_string(32, y, buf, WHITE, BLACK);
                lcd_st7789_draw_string(56, y, e->landlord_win ? "W" : "L", WHITE, BLACK);
                snprintf(buf, sizeof(buf), "%2u", (unsigned)e->points);
                lcd_st7789_draw_string(80, y, buf, WHITE, BLACK);
                snprintf(buf, sizeof(buf), "%4d", e->scores_after[0]);
                lcd_st7789_draw_string(104, y, buf, WHITE, BLACK);
                snprintf(buf, sizeof(buf), "%4d", e->scores_after[1]);
                lcd_st7789_draw_string(144, y, buf, WHITE, BLACK);
                snprintf(buf, sizeof(buf), "%4d", e->scores_after[2]);
                lcd_st7789_draw_string(184, y, buf, WHITE, BLACK);
            }
        }
    }

    /* 操作提示 */
    lcd_st7789_draw_string(0, LOG_HINT_Y, "短按翻页 长按退出 30秒", GREY, BLACK);
    lcd_unlock();
}
