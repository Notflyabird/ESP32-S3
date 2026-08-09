#include "voice_assets.h"

#include <stdio.h>
#include <string.h>

#include "audio_player.h"
#include "esp_heap_caps.h"
#include "esp_log.h"

static const char *TAG = "VOICE_ASSET";

/* ====================== 嵌入的 WAV 二进制符号 ======================
 * 由 src/CMakeLists.txt 的 target_add_binary_data 生成，
 * 符号名 = 文件名（点→下划线）。如 boot.wav -> _binary_boot_wav_start。
 */
extern const uint8_t _binary_boot_wav_start[],         _binary_boot_wav_end[];
extern const uint8_t _binary_im_here_wav_start[],      _binary_im_here_wav_end[];
extern const uint8_t _binary_score_reset_wav_start[],  _binary_score_reset_wav_end[];
extern const uint8_t _binary_nothing_undo_wav_start[], _binary_nothing_undo_wav_end[];
extern const uint8_t _binary_undo_timeout_wav_start[], _binary_undo_timeout_wav_end[];
extern const uint8_t _binary_p1_wav_start[],           _binary_p1_wav_end[];
extern const uint8_t _binary_p2_wav_start[],           _binary_p2_wav_end[];
extern const uint8_t _binary_p3_wav_start[],           _binary_p3_wav_end[];
extern const uint8_t _binary_landlord_wav_start[],     _binary_landlord_wav_end[];
extern const uint8_t _binary_win_wav_start[],          _binary_win_wav_end[];
extern const uint8_t _binary_lose_wav_start[],         _binary_lose_wav_end[];
extern const uint8_t _binary_fen_wav_start[],          _binary_fen_wav_end[];
extern const uint8_t _binary_cur_score_wav_start[],    _binary_cur_score_wav_end[];
extern const uint8_t _binary_undone_wav_start[],       _binary_undone_wav_end[];
extern const uint8_t _binary_restore_wav_start[],   _binary_restore_wav_end[];
extern const uint8_t _binary_view_log_wav_start[],  _binary_view_log_wav_end[];
extern const uint8_t _binary_clear_log_wav_start[], _binary_clear_log_wav_end[];
extern const uint8_t _binary_log_exit_wav_start[],  _binary_log_exit_wav_end[];
extern const uint8_t _binary_d0_wav_start[],           _binary_d0_wav_end[];
extern const uint8_t _binary_d1_wav_start[],           _binary_d1_wav_end[];
extern const uint8_t _binary_d2_wav_start[],           _binary_d2_wav_end[];
extern const uint8_t _binary_d3_wav_start[],           _binary_d3_wav_end[];
extern const uint8_t _binary_d4_wav_start[],           _binary_d4_wav_end[];
extern const uint8_t _binary_d5_wav_start[],           _binary_d5_wav_end[];
extern const uint8_t _binary_d6_wav_start[],           _binary_d6_wav_end[];
extern const uint8_t _binary_d7_wav_start[],           _binary_d7_wav_end[];
extern const uint8_t _binary_d8_wav_start[],           _binary_d8_wav_end[];
extern const uint8_t _binary_d9_wav_start[],           _binary_d9_wav_end[];
extern const uint8_t _binary_d10_wav_start[],          _binary_d10_wav_end[];
extern const uint8_t _binary_d100_wav_start[],         _binary_d100_wav_end[];
extern const uint8_t _binary_neg_wav_start[],          _binary_neg_wav_end[];

typedef struct {
    const uint8_t *start;
    const uint8_t *end;
    const char    *name;
} asset_entry_t;

static const asset_entry_t s_assets[VOICE_ASSET_COUNT] = {
    [VOICE_ASSET_BOOT]         = { _binary_boot_wav_start,         _binary_boot_wav_end,         "boot" },
    [VOICE_ASSET_IM_HERE]      = { _binary_im_here_wav_start,      _binary_im_here_wav_end,      "im_here" },
    [VOICE_ASSET_SCORE_RESET]  = { _binary_score_reset_wav_start,  _binary_score_reset_wav_end,  "score_reset" },
    [VOICE_ASSET_NOTHING_UNDO] = { _binary_nothing_undo_wav_start, _binary_nothing_undo_wav_end, "nothing_undo" },
    [VOICE_ASSET_UNDO_TIMEOUT] = { _binary_undo_timeout_wav_start, _binary_undo_timeout_wav_end, "undo_timeout" },
    [VOICE_ASSET_P1]           = { _binary_p1_wav_start,           _binary_p1_wav_end,           "p1" },
    [VOICE_ASSET_P2]           = { _binary_p2_wav_start,           _binary_p2_wav_end,           "p2" },
    [VOICE_ASSET_P3]           = { _binary_p3_wav_start,           _binary_p3_wav_end,           "p3" },
    [VOICE_ASSET_LANDLORD]     = { _binary_landlord_wav_start,     _binary_landlord_wav_end,     "landlord" },
    [VOICE_ASSET_WIN]          = { _binary_win_wav_start,          _binary_win_wav_end,          "win" },
    [VOICE_ASSET_LOSE]         = { _binary_lose_wav_start,         _binary_lose_wav_end,         "lose" },
    [VOICE_ASSET_FEN]          = { _binary_fen_wav_start,          _binary_fen_wav_end,          "fen" },
    [VOICE_ASSET_CUR_SCORE]    = { _binary_cur_score_wav_start,    _binary_cur_score_wav_end,    "cur_score" },
    [VOICE_ASSET_UNDONE]       = { _binary_undone_wav_start,       _binary_undone_wav_end,       "undone" },
    [VOICE_ASSET_RESTORE]      = { _binary_restore_wav_start,      _binary_restore_wav_end,      "restore" },
    [VOICE_ASSET_VIEW_LOG]     = { _binary_view_log_wav_start,     _binary_view_log_wav_end,     "view_log" },
    [VOICE_ASSET_CLEAR_LOG]    = { _binary_clear_log_wav_start,    _binary_clear_log_wav_end,    "clear_log" },
    [VOICE_ASSET_LOG_EXIT]     = { _binary_log_exit_wav_start,     _binary_log_exit_wav_end,     "log_exit" },
    [VOICE_ASSET_D0]           = { _binary_d0_wav_start,           _binary_d0_wav_end,           "d0" },
    [VOICE_ASSET_D1]           = { _binary_d1_wav_start,           _binary_d1_wav_end,           "d1" },
    [VOICE_ASSET_D2]           = { _binary_d2_wav_start,           _binary_d2_wav_end,           "d2" },
    [VOICE_ASSET_D3]           = { _binary_d3_wav_start,           _binary_d3_wav_end,           "d3" },
    [VOICE_ASSET_D4]           = { _binary_d4_wav_start,           _binary_d4_wav_end,           "d4" },
    [VOICE_ASSET_D5]           = { _binary_d5_wav_start,           _binary_d5_wav_end,           "d5" },
    [VOICE_ASSET_D6]           = { _binary_d6_wav_start,           _binary_d6_wav_end,           "d6" },
    [VOICE_ASSET_D7]           = { _binary_d7_wav_start,           _binary_d7_wav_end,           "d7" },
    [VOICE_ASSET_D8]           = { _binary_d8_wav_start,           _binary_d8_wav_end,           "d8" },
    [VOICE_ASSET_D9]           = { _binary_d9_wav_start,           _binary_d9_wav_end,           "d9" },
    [VOICE_ASSET_D10]          = { _binary_d10_wav_start,          _binary_d10_wav_end,          "d10" },
    [VOICE_ASSET_D100]         = { _binary_d100_wav_start,         _binary_d100_wav_end,         "d100" },
    [VOICE_ASSET_NEG]          = { _binary_neg_wav_start,          _binary_neg_wav_end,          "neg" },
};

/* ====================== WAV 头解析（复用 audio_player 逻辑）====================== */
#pragma pack(push, 1)
typedef struct {
    char     riff_id[4];
    uint32_t file_size;
    char     wave_id[4];
    char     fmt_id[4];
    uint32_t fmt_size;
    uint16_t audio_format;
    uint16_t channels;
    uint32_t sample_rate;
    uint32_t byte_rate;
    uint16_t block_align;
    uint16_t bits_per_sample;
} wav_header_t;

typedef struct {
    char     data_id[4];
    uint32_t data_size;
} wav_data_chunk_t;
#pragma pack(pop)

/* 解析嵌入的 WAV，返回 data chunk 内 PCM 指针与采样数。 */
static const int16_t *parse_wav(const uint8_t *start, const uint8_t *end,
                                size_t *out_samples, const char *name)
{
    size_t buf_len = (size_t)(end - start);
    if (buf_len < sizeof(wav_header_t) + sizeof(wav_data_chunk_t)) {
        ESP_LOGE(TAG, "%s: too small (%u bytes)", name, (unsigned)buf_len);
        return NULL;
    }

    const wav_header_t *hdr = (const wav_header_t *)start;
    if (memcmp(hdr->riff_id, "RIFF", 4) != 0 ||
        memcmp(hdr->wave_id, "WAVE", 4) != 0 ||
        memcmp(hdr->fmt_id, "fmt ", 4) != 0) {
        ESP_LOGE(TAG, "%s: not a valid WAV", name);
        return NULL;
    }
    if (hdr->audio_format != 1) {
        ESP_LOGE(TAG, "%s: not PCM (format=%u)", name, hdr->audio_format);
        return NULL;
    }
    if (hdr->channels != 1 || hdr->bits_per_sample != 16) {
        ESP_LOGE(TAG, "%s: unsupported %u ch, %u bits", name,
                 hdr->channels, hdr->bits_per_sample);
        return NULL;
    }

    /* 跳过 fmt 扩展字段后搜索 data chunk */
    size_t offset = sizeof(wav_header_t);
    if (hdr->fmt_size > 16) {
        offset += hdr->fmt_size - 16;
    }

    const wav_data_chunk_t *dc = NULL;
    while (offset + sizeof(wav_data_chunk_t) <= buf_len) {
        const wav_data_chunk_t *c = (const wav_data_chunk_t *)(start + offset);
        if (memcmp(c->data_id, "data", 4) == 0) {
            dc = c;
            break;
        }
        offset += sizeof(wav_data_chunk_t) + c->data_size;
    }
    if (dc == NULL) {
        ESP_LOGE(TAG, "%s: no data chunk", name);
        return NULL;
    }

    const int16_t *pcm = (const int16_t *)(dc + 1);
    size_t pcm_bytes = dc->data_size;
    /* 防止 data_size 声明超出实际嵌入范围 */
    if ((const uint8_t *)pcm + pcm_bytes > end) {
        pcm_bytes = (size_t)(end - (const uint8_t *)pcm);
    }
    *out_samples = pcm_bytes / sizeof(int16_t);
    return pcm;
}

const int16_t *voice_asset_pcm(voice_asset_id_t id, size_t *sample_count)
{
    if (id < 0 || id >= VOICE_ASSET_COUNT) {
        return NULL;
    }
    const asset_entry_t *e = &s_assets[id];
    size_t n = 0;
    const int16_t *pcm = parse_wav(e->start, e->end, &n, e->name);
    if (sample_count) {
        *sample_count = n;
    }
    return pcm;
}

/* ====================== 中文读数 ====================== */
#define DIGIT_ID(d) ((voice_asset_id_t)(VOICE_ASSET_D0 + (d)))

static size_t append_one(voice_asset_id_t *ids, size_t count, size_t cap, voice_asset_id_t id)
{
    if (count < cap) {
        ids[count] = id;
    }
    return count + 1;
}

size_t voice_assets_append_number(voice_asset_id_t *ids, size_t count,
                                  size_t capacity, int value)
{
    if (value < 0) {
        count = append_one(ids, count, capacity, VOICE_ASSET_NEG);
        value = -value;
    }
    if (value == 0) {
        return append_one(ids, count, capacity, VOICE_ASSET_D0);
    }
    if (value > 999) {
        /* 超界回退：逐位读（含零）*/
        char buf[12];
        snprintf(buf, sizeof(buf), "%d", value);
        for (int i = 0; buf[i] != '\0'; ++i) {
            int d = buf[i] - '0';
            count = append_one(ids, count, capacity, DIGIT_ID(d));
        }
        return count;
    }

    int h = value / 100;
    if (h > 0) {
        count = append_one(ids, count, capacity, DIGIT_ID(h));
        count = append_one(ids, count, capacity, VOICE_ASSET_D100);
        int r = value % 100;
        int t = r / 10;
        int o = r % 10;
        if (t == 0) {
            if (o > 0) {
                /* 101 = 一百零一 */
                count = append_one(ids, count, capacity, VOICE_ASSET_D0);
            }
        } else {
            /* 百位存在时十位为 1 也读：110 = 一百一十 */
            count = append_one(ids, count, capacity, DIGIT_ID(t));
            count = append_one(ids, count, capacity, VOICE_ASSET_D10);
        }
        if (o > 0) {
            count = append_one(ids, count, capacity, DIGIT_ID(o));
        }
        return count;
    }

    /* 无百位：1..99 */
    int t = value / 10;
    int o = value % 10;
    if (t == 0) {
        /* 1..9 */
        count = append_one(ids, count, capacity, DIGIT_ID(o));
    } else if (t == 1) {
        /* 10..19：读"十"不读"一十" */
        count = append_one(ids, count, capacity, VOICE_ASSET_D10);
        if (o > 0) {
            count = append_one(ids, count, capacity, DIGIT_ID(o));
        }
    } else {
        /* 20..99 */
        count = append_one(ids, count, capacity, DIGIT_ID(t));
        count = append_one(ids, count, capacity, VOICE_ASSET_D10);
        if (o > 0) {
            count = append_one(ids, count, capacity, DIGIT_ID(o));
        }
    }
    return count;
}

/* ====================== 序列拼接播放 ====================== */
void voice_play_sequence(const voice_asset_id_t *ids, size_t count)
{
    if (count == 0 || count > VOICE_SEQ_MAX) {
        ESP_LOGW(TAG, "sequence count out of range: %u", (unsigned)count);
        return;
    }

    /* 收集各素材 PCM 指针与采样数，并计算总长 */
    const int16_t *seg_pcm[VOICE_SEQ_MAX];
    size_t seg_n[VOICE_SEQ_MAX];
    size_t total = 0;
    for (size_t i = 0; i < count; ++i) {
        size_t n = 0;
        seg_pcm[i] = voice_asset_pcm(ids[i], &n);
        seg_n[i] = n;
        total += n;
        if (i + 1 < count) {
            total += VOICE_GAP_SAMPLES;
        }
    }
    if (total == 0) {
        return;
    }

    /* 分配 PSRAM 拼接缓冲 */
    int16_t *buf = (int16_t *)heap_caps_malloc(total * sizeof(int16_t),
                                               MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (buf == NULL) {
        /* 回退：逐素材播放 + 静态零间隙，维持 DSP 连续 */
        static int16_t s_zero_gap[VOICE_GAP_SAMPLES];  /* BSS 零初始化 */
        ESP_LOGW(TAG, "PSRAM alloc failed (%u samples), fallback per-asset", (unsigned)total);
        for (size_t i = 0; i < count; ++i) {
            if (seg_pcm[i] != NULL && seg_n[i] > 0) {
                audio_play_pcm(seg_pcm[i], seg_n[i]);
            }
            if (i + 1 < count) {
                audio_play_pcm(s_zero_gap, VOICE_GAP_SAMPLES);
            }
        }
        return;
    }

    /* 拼接：整体清零（间隙为零），再逐素材 memcpy */
    memset(buf, 0, total * sizeof(int16_t));
    size_t off = 0;
    for (size_t i = 0; i < count; ++i) {
        if (seg_pcm[i] != NULL && seg_n[i] > 0) {
            memcpy(buf + off, seg_pcm[i], seg_n[i] * sizeof(int16_t));
            off += seg_n[i];
        }
        if (i + 1 < count) {
            off += VOICE_GAP_SAMPLES;  /* 已为零 */
        }
    }

    audio_play_pcm(buf, total);
    free(buf);
}
