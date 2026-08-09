#pragma once

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ====================== 预生成语音素材（嵌入 flash）======================
 * 由 voice_assets/generate_assets.py 用 edge-tts 生成，16kHz mono 16bit PCM。
 * 运行时按词拼接播放，替代 esp-tts 合成。
 */

typedef enum {
    /* 固定话术（整句）*/
    VOICE_ASSET_BOOT = 0,
    VOICE_ASSET_IM_HERE,
    VOICE_ASSET_SCORE_RESET,
    VOICE_ASSET_NOTHING_UNDO,
    VOICE_ASSET_UNDO_TIMEOUT,
    /* 玩家 */
    VOICE_ASSET_P1,
    VOICE_ASSET_P2,
    VOICE_ASSET_P3,
    /* 动作词 */
    VOICE_ASSET_LANDLORD,
    VOICE_ASSET_WIN,
    VOICE_ASSET_LOSE,
    VOICE_ASSET_FEN,
    /* 模板词 */
    VOICE_ASSET_CUR_SCORE,
    VOICE_ASSET_UNDONE,
    VOICE_ASSET_RESTORE,
    /* 日志相关 */
    VOICE_ASSET_VIEW_LOG,
    VOICE_ASSET_CLEAR_LOG,
    VOICE_ASSET_LOG_EXIT,
    /* 识别失败提示 */
    VOICE_ASSET_UNCLEAR,
    /* 数字（运行时拼接中文读数）*/
    VOICE_ASSET_D0,
    VOICE_ASSET_D1,
    VOICE_ASSET_D2,
    VOICE_ASSET_D3,
    VOICE_ASSET_D4,
    VOICE_ASSET_D5,
    VOICE_ASSET_D6,
    VOICE_ASSET_D7,
    VOICE_ASSET_D8,
    VOICE_ASSET_D9,
    VOICE_ASSET_D10,    /* 十 */
    VOICE_ASSET_D100,   /* 百 */
    VOICE_ASSET_NEG,    /* 负 */
    VOICE_ASSET_COUNT
} voice_asset_id_t;

/* 词间静音：拼接时在相邻素材间插入的零样本数。
 * 60ms @ 16kHz = 960 样本。与 audio_player 的 DSP_INPUT_RATE 一致。*/
#define VOICE_GAP_MS        60
#define VOICE_INPUT_RATE    16000
#define VOICE_GAP_SAMPLES   (VOICE_INPUT_RATE * VOICE_GAP_MS / 1000)

/* 单句最大素材数（最坏：撤销非重置 + 三个 -999 分 ≈ 41，留余量）*/
#define VOICE_SEQ_MAX       48

/**
 * @brief 取某素材的 PCM 数据指针（解析 WAV 头，指向 flash 内 data chunk）。
 *
 * @param id            素材 ID。
 * @param sample_count  输出：PCM 采样点数（不是字节数）。
 * @return 指向 int16 PCM 的只读指针（位于 flash，无需释放）。
 */
const int16_t *voice_asset_pcm(voice_asset_id_t id, size_t *sample_count);

/**
 * @brief 播放一个素材序列（阻塞直到所有数据写入 I2S DMA）。
 *
 * 将各素材 PCM 与词间静音预拼接进一个 PSRAM 缓冲，单次 audio_play_pcm 播放，
 * DSP 链全程不重置（句首由调用方 audio_player_reset_dsp 重置）。
 * PSRAM 分配失败时回退为逐素材 audio_play_pcm + 零间隙。
 *
 * @param ids   素材 ID 数组。
 * @param count 素材数量。
 */
void voice_play_sequence(const voice_asset_id_t *ids, size_t count);

/**
 * @brief 向 ids 数组追加中文读数素材（支持 -999..999，超界逐位读）。
 *
 * 规则：负数前加 NEG；0→D0；10→十（非一十）；20→二十；100→一百；
 *       101→一百零一；110→一百一十；>999 逐位读。
 *
 * @param ids       素材数组。
 * @param count     当前已用数量。
 * @param capacity  数组容量。
 * @param value     要读的整数。
 * @return 追加后的新数量。
 */
size_t voice_assets_append_number(voice_asset_id_t *ids, size_t count,
                                  size_t capacity, int value);

#ifdef __cplusplus
}
#endif
