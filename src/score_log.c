#include "score_log.h"

#include <stdio.h>
#include <string.h>

#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "lcd_ui.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "scorekeeper.h"

static const char *TAG = "SCORE_LOG";

#define NVS_NAMESPACE  "ddz_score"
#define NVS_KEY_LOG    "log"
#define NVS_KEY_SCORES "scores"

/* ====================== NVS blob 布局（packed，与 flash 二进制严格一致）====================== */
typedef struct __attribute__((packed)) {
    uint16_t round_no_next;     /* 下一条要分配的局号 */
    uint16_t write_idx;         /* 下一条写入位置 0..255 */
    uint16_t count;             /* 当前有效条数 0..256 */
    uint16_t reserved;          /* 对齐保留 */
    score_log_entry_t entries[SCORE_LOG_CAPACITY];
} score_log_blob_t;             /* 8 + 256×14 = 3592 字节 */

typedef struct __attribute__((packed)) {
    int32_t scores[3];
    int32_t landlord;           /* 0=未指定, 1..3 */
} score_cur_blob_t;             /* 16 字节 */

/* ====================== 视图状态（仅 RAM）====================== */
typedef struct {
    bool     active;
    uint8_t  page_index;
    uint8_t  num_pages;
} score_log_view_t;

/* ====================== 静态状态 ====================== */
static score_log_blob_t   s_log;
static score_log_view_t   s_view;
static nvs_handle_t       s_nvs = 0;
static SemaphoreHandle_t  s_log_lock = NULL;
static bool               s_persist_disabled = false;

/* ====================== 内部：NVS 写 ====================== */
static void persist_log_locked(void)
{
    if (s_persist_disabled) return;
    esp_err_t err = nvs_set_blob(s_nvs, NVS_KEY_LOG, &s_log, sizeof(s_log));
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "nvs_set_blob(log) failed: %s", esp_err_to_name(err));
    }
    err = nvs_commit(s_nvs);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "nvs_commit(log) failed: %s", esp_err_to_name(err));
    }
}

static void persist_scores_locked(int s1, int s2, int s3, int landlord)
{
    if (s_persist_disabled) return;
    score_cur_blob_t cur;
    cur.scores[0] = s1;
    cur.scores[1] = s2;
    cur.scores[2] = s3;
    cur.landlord  = landlord;
    esp_err_t err = nvs_set_blob(s_nvs, NVS_KEY_SCORES, &cur, sizeof(cur));
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "nvs_set_blob(scores) failed: %s", esp_err_to_name(err));
    }
    err = nvs_commit(s_nvs);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "nvs_commit(scores) failed: %s", esp_err_to_name(err));
    }
}

/* ====================== 初始化 ====================== */
bool score_log_init(void)
{
    s_log_lock = xSemaphoreCreateMutex();
    if (s_log_lock == NULL) {
        ESP_LOGE(TAG, "Failed to create log lock");
        return false;
    }

    memset(&s_log, 0, sizeof(s_log));
    s_log.round_no_next = 1;
    memset(&s_view, 0, sizeof(s_view));

    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &s_nvs);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "nvs_open failed: %s — degrade to RAM-only", esp_err_to_name(err));
        s_persist_disabled = true;
        int zero[3] = {0, 0, 0};
        scorekeeper_restore_state(zero, 0);
        return false;
    }

    /* 恢复当前分数 */
    score_cur_blob_t cur;
    memset(&cur, 0, sizeof(cur));
    size_t sz = sizeof(cur);
    if (nvs_get_blob(s_nvs, NVS_KEY_SCORES, &cur, &sz) == ESP_OK && sz == sizeof(cur)) {
        int sc[3] = {(int)cur.scores[0], (int)cur.scores[1], (int)cur.scores[2]};
        scorekeeper_restore_state(sc, (int)cur.landlord);
        ESP_LOGI(TAG, "Restored scores: P1=%d P2=%d P3=%d LL=%d",
                 sc[0], sc[1], sc[2], (int)cur.landlord);
    } else {
        int zero[3] = {0, 0, 0};
        scorekeeper_restore_state(zero, 0);
        ESP_LOGI(TAG, "No saved scores, starting fresh");
    }

    /* 恢复日志缓冲 */
    sz = sizeof(s_log);
    if (nvs_get_blob(s_nvs, NVS_KEY_LOG, &s_log, &sz) == ESP_OK && sz == sizeof(s_log)) {
        ESP_LOGI(TAG, "Restored log: %u entries", (unsigned)s_log.count);
    } else {
        memset(&s_log, 0, sizeof(s_log));
        s_log.round_no_next = 1;
        ESP_LOGI(TAG, "No saved log, starting fresh");
    }

    ESP_LOGI(TAG, "init done (persist_disabled=%d)", s_persist_disabled);
    return true;
}

/* ====================== 写入 ====================== */
uint16_t score_log_next_round_no(void)
{
    xSemaphoreTake(s_log_lock, portMAX_DELAY);
    uint16_t rnd = s_log.round_no_next;
    s_log.round_no_next = (uint16_t)((rnd % 65535u) + 1u);
    xSemaphoreGive(s_log_lock);
    return rnd;
}

static void append_entry_locked(const score_log_entry_t *e)
{
    s_log.entries[s_log.write_idx] = *e;
    s_log.write_idx = (uint16_t)((s_log.write_idx + 1) % SCORE_LOG_CAPACITY);
    if (s_log.count < SCORE_LOG_CAPACITY) {
        s_log.count++;
    }
    persist_log_locked();
}

void score_log_append_round(uint16_t rnd, uint8_t player, uint8_t landlord,
                            bool win, uint8_t points, int s1, int s2, int s3)
{
    score_log_entry_t e;
    e.round_no      = rnd;
    e.op            = LOG_OP_ROUND;
    e.player        = player;
    e.landlord      = landlord;
    e.landlord_win  = win ? 1 : 0;
    e.points        = points;
    e.scores_after[0] = (int16_t)s1;
    e.scores_after[1] = (int16_t)s2;
    e.scores_after[2] = (int16_t)s3;

    xSemaphoreTake(s_log_lock, portMAX_DELAY);
    append_entry_locked(&e);
    xSemaphoreGive(s_log_lock);
}

void score_log_append_reset(uint16_t rnd, int s1, int s2, int s3)
{
    score_log_entry_t e;
    e.round_no      = rnd;
    e.op            = LOG_OP_RESET;
    e.player        = 0;
    e.landlord      = 0;
    e.landlord_win  = 0;
    e.points        = 0;
    e.scores_after[0] = (int16_t)s1;
    e.scores_after[1] = (int16_t)s2;
    e.scores_after[2] = (int16_t)s3;

    xSemaphoreTake(s_log_lock, portMAX_DELAY);
    append_entry_locked(&e);
    xSemaphoreGive(s_log_lock);
}

void score_log_persist_scores_locked(int s1, int s2, int s3, int landlord)
{
    /* 调用方已持 s_score_lock；此处仅取 s_log_lock 写 NVS */
    xSemaphoreTake(s_log_lock, portMAX_DELAY);
    persist_scores_locked(s1, s2, s3, landlord);
    xSemaphoreGive(s_log_lock);
}

void score_log_clear_all(void)
{
    xSemaphoreTake(s_log_lock, portMAX_DELAY);
    memset(&s_log, 0, sizeof(s_log));
    s_log.round_no_next = 1;
    s_view.active = false;
    s_view.page_index = 0;
    s_view.num_pages = 0;

    if (!s_persist_disabled) {
        nvs_erase_key(s_nvs, NVS_KEY_LOG);
        score_cur_blob_t cur;
        memset(&cur, 0, sizeof(cur));
        nvs_set_blob(s_nvs, NVS_KEY_SCORES, &cur, sizeof(cur));
        nvs_commit(s_nvs);
    }
    xSemaphoreGive(s_log_lock);
    ESP_LOGI(TAG, "all logs and scores cleared");
}

/* ====================== 视图：取一页数据（newest 在顶）====================== */
static uint8_t fill_page(uint8_t page_idx, score_log_entry_t *out)
{
    uint16_t count = s_log.count;
    if (count == 0) {
        return 0;
    }
    uint16_t skip = (uint16_t)(page_idx * SCORE_LOG_PAGE_ROWS);
    if (skip >= count) {
        return 0;
    }
    uint16_t avail = (uint16_t)(count - skip);
    uint8_t n = (avail < SCORE_LOG_PAGE_ROWS) ? (uint8_t)avail : SCORE_LOG_PAGE_ROWS;
    /* k=0 为最新条目，位于 (write_idx-1+CAP)%CAP */
    for (uint8_t i = 0; i < n; i++) {
        uint16_t k = (uint16_t)(skip + i);
        uint16_t idx = (uint16_t)((s_log.write_idx + SCORE_LOG_CAPACITY - 1 - k) % SCORE_LOG_CAPACITY);
        out[i] = s_log.entries[idx];
    }
    return n;
}

bool score_log_view_is_active(void)
{
    xSemaphoreTake(s_log_lock, portMAX_DELAY);
    bool a = s_view.active;
    xSemaphoreGive(s_log_lock);
    return a;
}

void score_log_view_enter(void)
{
    xSemaphoreTake(s_log_lock, portMAX_DELAY);
    s_view.active = true;
    s_view.page_index = 0;
    uint16_t count = s_log.count;
    s_view.num_pages = (uint8_t)((count + SCORE_LOG_PAGE_ROWS - 1) / SCORE_LOG_PAGE_ROWS);
    if (s_view.num_pages == 0) {
        s_view.num_pages = 1;   /* 空日志也显示一页 "No records" */
    }
    score_log_entry_t page[SCORE_LOG_PAGE_ROWS];
    uint8_t n = fill_page(0, page);
    uint8_t np = s_view.num_pages;
    xSemaphoreGive(s_log_lock);

    lcd_ui_log_draw_page(0, np, page, n);
}

void score_log_view_next_page(void)
{
    xSemaphoreTake(s_log_lock, portMAX_DELAY);
    if (!s_view.active) {
        xSemaphoreGive(s_log_lock);
        return;
    }
    if (s_view.num_pages > 1) {
        s_view.page_index = (uint8_t)((s_view.page_index + 1) % s_view.num_pages);
    }
    score_log_entry_t page[SCORE_LOG_PAGE_ROWS];
    uint8_t n = fill_page(s_view.page_index, page);
    uint8_t pi = s_view.page_index;
    uint8_t np = s_view.num_pages;
    xSemaphoreGive(s_log_lock);

    lcd_ui_log_draw_page(pi, np, page, n);
}

void score_log_view_exit(void)
{
    xSemaphoreTake(s_log_lock, portMAX_DELAY);
    if (!s_view.active) {
        xSemaphoreGive(s_log_lock);
        return;   /* 幂等 */
    }
    s_view.active = false;
    xSemaphoreGive(s_log_lock);

    /* 重绘主页（s_lcd_lock 在 lcd_ui 内部获取）*/
    int s1, s2, s3, landlord;
    scorekeeper_get_scores(&s1, &s2, &s3, &landlord);
    lcd_ui_init_page();
    lcd_ui_update((uint8_t)landlord, s1, s2, s3, "就绪 你好小鑫");
}
