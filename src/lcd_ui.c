#include "lcd_ui.h"

#include <stdbool.h>
#include <stdio.h>

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

/* Title — centered, 17 chars × 8 px = 136 px, (240-136)/2 ≈ 52 */
#define TITLE_X    52
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

/* Status line */
#define ST_LABEL_X  12
#define ST_TEXT_X   82
#define ST_Y        192

/* Total line */
#define TT_LABEL_X  12
#define TT_TEXT_X   68
#define TT_Y        232

/* Hint */
#define HINT_X      8
#define HINT_Y      285

/* Score field clearing rectangle */
#define SCORE_BG_W  80
#define SCORE_BG_H  16

/* Label width for erase (e.g. "P1:" = 3 chars = 24 px, "Status:" = 7*8=56) */
#define LABEL_ERASE_W 72

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
    lcd_st7789_fill_screen(BLACK);

    /* Title */
    lcd_st7789_draw_string(TITLE_X, TITLE_Y, "DDZ ScoreKeeper", YELLOW, BLACK);

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

    lcd_st7789_draw_string(ST_LABEL_X, ST_Y, "Status:", WHITE, BLACK);
    lcd_st7789_draw_string(TT_LABEL_X, TT_Y, "Total:",  WHITE, BLACK);

    /* Hint – grey */
    lcd_st7789_draw_string(HINT_X, HINT_Y, "Voice CMD", GREY, BLACK);
}

void lcd_ui_update(uint8_t landlord_no,
                   int s1, int s2, int s3,
                   const char *status)
{
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
}
