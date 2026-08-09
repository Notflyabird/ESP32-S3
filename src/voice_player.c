#include "voice_player.h"

#include <string.h>

#include "audio_player.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "voice_assets.h"

static const char *TAG = "VOICE_PLAYER";

#define VOICE_TASK_STACK_SIZE  8192
#define VOICE_TASK_PRIORITY    5
#define VOICE_QUEUE_LENGTH     4
#define VOICE_DRAIN_MS         200   /* DMA 缓冲 128ms 排空 + 余量 */

/* SR 麦克风采集暂停标志：播放期间置 true，feed_task 循环等待。
 * 定义在此（原 tts_player.c），speech_recognition.c 以 extern 引用。 */
volatile bool g_sr_paused = false;

static volatile bool s_playing = false;
static QueueHandle_t s_queue = NULL;
static TaskHandle_t s_task = NULL;

/* ====================== 任务描述 ====================== */
typedef enum {
    VOICE_JOB_BOOT,
    VOICE_JOB_IM_HERE,
    VOICE_JOB_RESET,
    VOICE_JOB_UNDO_NONE,
    VOICE_JOB_UNDO_TIMEOUT,
    VOICE_JOB_SCORE_UPDATE,
    VOICE_JOB_QUERY,
    VOICE_JOB_UNDO_RESULT,
    VOICE_JOB_VIEW_LOG,
    VOICE_JOB_CLEAR_LOG,
    VOICE_JOB_LOG_EXIT,
} voice_job_type_t;

typedef struct {
    voice_job_type_t type;
    /* 计分 / 撤销操作字段 */
    uint8_t  player;
    bool     landlord_win;
    int      points;
    /* 撤销结果字段 */
    bool     was_reset;
    uint8_t  op_player;
    bool     op_landlord_win;
    int      op_points;
    /* 当前分数（查询 / 撤销） */
    int      s1, s2, s3;
} voice_job_t;

/* ====================== 序列组装 ====================== */
static void build_score_update(voice_asset_id_t *ids, size_t *n,
                               uint8_t player, bool win, int points)
{
    *n = 0;
    ids[(*n)++] = (voice_asset_id_t)(VOICE_ASSET_P1 + (player - 1));
    ids[(*n)++] = VOICE_ASSET_LANDLORD;
    ids[(*n)++] = win ? VOICE_ASSET_WIN : VOICE_ASSET_LOSE;
    *n = voice_assets_append_number(ids, *n, VOICE_SEQ_MAX, points);
    ids[(*n)++] = VOICE_ASSET_FEN;
}

static void build_scores_block(voice_asset_id_t *ids, size_t *n,
                               int s1, int s2, int s3)
{
    int scores[3] = {s1, s2, s3};
    for (int i = 0; i < 3; ++i) {
        ids[(*n)++] = (voice_asset_id_t)(VOICE_ASSET_P1 + i);
        *n = voice_assets_append_number(ids, *n, VOICE_SEQ_MAX, scores[i]);
        ids[(*n)++] = VOICE_ASSET_FEN;
    }
}

static void build_query(voice_asset_id_t *ids, size_t *n, int s1, int s2, int s3)
{
    *n = 0;
    ids[(*n)++] = VOICE_ASSET_CUR_SCORE;
    build_scores_block(ids, n, s1, s2, s3);
}

static void build_undo_result(voice_asset_id_t *ids, size_t *n,
                              bool was_reset, uint8_t op_player,
                              bool op_win, int op_points,
                              int s1, int s2, int s3)
{
    *n = 0;
    ids[(*n)++] = VOICE_ASSET_UNDONE;
    if (!was_reset) {
        /* 已撤销 + 被撤销的操作 + 当前分数 */
        ids[(*n)++] = (voice_asset_id_t)(VOICE_ASSET_P1 + (op_player - 1));
        ids[(*n)++] = VOICE_ASSET_LANDLORD;
        ids[(*n)++] = op_win ? VOICE_ASSET_WIN : VOICE_ASSET_LOSE;
        *n = voice_assets_append_number(ids, *n, VOICE_SEQ_MAX, op_points);
        ids[(*n)++] = VOICE_ASSET_FEN;
        ids[(*n)++] = VOICE_ASSET_CUR_SCORE;
    } else {
        /* 已撤销 + 分数恢复为 */
        ids[(*n)++] = VOICE_ASSET_RESTORE;
    }
    build_scores_block(ids, n, s1, s2, s3);
}

/* ====================== 任务体 ====================== */
static void voice_task(void *arg)
{
    (void)arg;
    ESP_LOGI(TAG, "voice task started on core %d", xPortGetCoreID());

    voice_job_t job;
    while (xQueueReceive(s_queue, &job, portMAX_DELAY) == pdTRUE) {
        s_playing = true;

        /* 暂停 SR 麦克风采集，避免回授误识别 */
        g_sr_paused = true;
        vTaskDelay(pdMS_TO_TICKS(50));

        audio_set_mute(false);
        audio_player_reset_dsp();

        voice_asset_id_t ids[VOICE_SEQ_MAX];
        size_t n = 0;
        switch (job.type) {
        case VOICE_JOB_BOOT:
            ids[n++] = VOICE_ASSET_BOOT;
            break;
        case VOICE_JOB_IM_HERE:
            ids[n++] = VOICE_ASSET_IM_HERE;
            break;
        case VOICE_JOB_RESET:
            ids[n++] = VOICE_ASSET_SCORE_RESET;
            break;
        case VOICE_JOB_UNDO_NONE:
            ids[n++] = VOICE_ASSET_NOTHING_UNDO;
            break;
        case VOICE_JOB_UNDO_TIMEOUT:
            ids[n++] = VOICE_ASSET_UNDO_TIMEOUT;
            break;
        case VOICE_JOB_SCORE_UPDATE:
            build_score_update(ids, &n, job.player, job.landlord_win, job.points);
            break;
        case VOICE_JOB_QUERY:
            build_query(ids, &n, job.s1, job.s2, job.s3);
            break;
        case VOICE_JOB_UNDO_RESULT:
            build_undo_result(ids, &n, job.was_reset, job.op_player,
                              job.op_landlord_win, job.op_points,
                              job.s1, job.s2, job.s3);
            break;
        case VOICE_JOB_VIEW_LOG:
            ids[n++] = VOICE_ASSET_VIEW_LOG;
            break;
        case VOICE_JOB_CLEAR_LOG:
            ids[n++] = VOICE_ASSET_CLEAR_LOG;
            break;
        case VOICE_JOB_LOG_EXIT:
            ids[n++] = VOICE_ASSET_LOG_EXIT;
            break;
        }

        voice_play_sequence(ids, n);

        /* 排空 DMA 缓冲（128ms）后静音 */
        vTaskDelay(pdMS_TO_TICKS(VOICE_DRAIN_MS));
        audio_set_mute(true);

        g_sr_paused = false;
        s_playing = false;
    }
}

/* ====================== 公共 API ====================== */
static bool enqueue(voice_job_t *job)
{
    s_playing = true;
    if (xQueueSend(s_queue, job, pdMS_TO_TICKS(100)) != pdTRUE) {
        ESP_LOGW(TAG, "voice queue full, dropping job %d", job->type);
        s_playing = false;
        return false;
    }
    return true;
}

bool voice_player_init(void)
{
    s_queue = xQueueCreate(VOICE_QUEUE_LENGTH, sizeof(voice_job_t));
    if (s_queue == NULL) {
        ESP_LOGE(TAG, "Failed to create voice queue");
        return false;
    }

    if (xTaskCreatePinnedToCore(voice_task, "voice_task", VOICE_TASK_STACK_SIZE,
                                NULL, VOICE_TASK_PRIORITY, &s_task, 1) != pdPASS) {
        ESP_LOGE(TAG, "Failed to create voice task");
        vQueueDelete(s_queue);
        s_queue = NULL;
        return false;
    }

    ESP_LOGI(TAG, "voice player ready (queue=%d, stack=%d, core=1)",
             VOICE_QUEUE_LENGTH, VOICE_TASK_STACK_SIZE);
    return true;
}

bool voice_is_playing(void)
{
    return s_playing;
}

void voice_player_deinit(void)
{
    if (s_task) {
        vTaskDelete(s_task);
        s_task = NULL;
    }
    if (s_queue) {
        vQueueDelete(s_queue);
        s_queue = NULL;
    }
    ESP_LOGI(TAG, "voice player deinitialized");
}

void voice_speak_boot(void)
{
    voice_job_t j = { .type = VOICE_JOB_BOOT };
    enqueue(&j);
}

void voice_speak_im_here(void)
{
    voice_job_t j = { .type = VOICE_JOB_IM_HERE };
    enqueue(&j);
}

void voice_speak_reset(void)
{
    voice_job_t j = { .type = VOICE_JOB_RESET };
    enqueue(&j);
}

void voice_speak_undo_none(void)
{
    voice_job_t j = { .type = VOICE_JOB_UNDO_NONE };
    enqueue(&j);
}

void voice_speak_undo_timeout(void)
{
    voice_job_t j = { .type = VOICE_JOB_UNDO_TIMEOUT };
    enqueue(&j);
}

void voice_speak_score_update(uint8_t player, bool landlord_win, int points)
{
    voice_job_t j = {
        .type = VOICE_JOB_SCORE_UPDATE,
        .player = player,
        .landlord_win = landlord_win,
        .points = points,
    };
    enqueue(&j);
}

void voice_speak_query(int s1, int s2, int s3)
{
    voice_job_t j = {
        .type = VOICE_JOB_QUERY,
        .s1 = s1, .s2 = s2, .s3 = s3,
    };
    enqueue(&j);
}

void voice_speak_undo_result(bool was_reset, uint8_t op_player,
                             bool op_landlord_win, int op_points,
                             int s1, int s2, int s3)
{
    voice_job_t j = {
        .type = VOICE_JOB_UNDO_RESULT,
        .was_reset = was_reset,
        .op_player = op_player,
        .op_landlord_win = op_landlord_win,
        .op_points = op_points,
        .s1 = s1, .s2 = s2, .s3 = s3,
    };
    enqueue(&j);
}

void voice_speak_view_log(void)
{
    voice_job_t j = { .type = VOICE_JOB_VIEW_LOG };
    enqueue(&j);
}

void voice_speak_clear_log(void)
{
    voice_job_t j = { .type = VOICE_JOB_CLEAR_LOG };
    enqueue(&j);
}

void voice_speak_log_exit(void)
{
    voice_job_t j = { .type = VOICE_JOB_LOG_EXIT };
    enqueue(&j);
}
