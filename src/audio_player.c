#include "audio_player.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "driver/gpio.h"
#include "driver/i2s_std.h"
#include "esp_check.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "AUDIO_PLAY";

/* ====================== 引脚定义 ====================== */
#define I2S1_BCLK  GPIO_NUM_15
#define I2S1_LRCLK GPIO_NUM_16
#define I2S1_DOUT  GPIO_NUM_17
#define SD_MUTE_PIN GPIO_NUM_18

/* ====================== 音频参数 ====================== */
#define SAMPLE_RATE 16000
#define I2S_BITS    I2S_DATA_BIT_WIDTH_16BIT

/* ====================== WAV 头解析 ====================== */
#pragma pack(push, 1)
typedef struct {
    char     riff_id[4];     // "RIFF"
    uint32_t file_size;      // 文件总长 - 8
    char     wave_id[4];     // "WAVE"
    char     fmt_id[4];      // "fmt "
    uint32_t fmt_size;       // 16 for PCM
    uint16_t audio_format;   // 1 = PCM
    uint16_t channels;       // 声道数
    uint32_t sample_rate;    // 采样率
    uint32_t byte_rate;      // 每秒字节数
    uint16_t block_align;    // 每帧字节数
    uint16_t bits_per_sample;// 位深度
} wav_header_t;

typedef struct {
    char     data_id[4];     // "data"
    uint32_t data_size;      // PCM 数据字节数
} wav_data_chunk_t;
#pragma pack(pop)

/* ====================== 播放状态 ====================== */
static bool s_playing = false;
static size_t s_pcm_bytes = 0;  // 当前播放 PCM 数据字节数（用于计算时长）

/* ---------- I2S1 通道句柄 ---------- */
static i2s_chan_handle_t s_tx_chan = NULL;

/* ================================================================== */
/*  I2S 初始化                                                         */
/* ================================================================== */
void audio_player_init(void)
{
    /* --- 配置 SD 静音引脚 --- */
    gpio_config_t sd_conf = {
        .pin_bit_mask = (1ULL << SD_MUTE_PIN),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    ESP_ERROR_CHECK(gpio_config(&sd_conf));
    gpio_set_level(SD_MUTE_PIN, 1);  // 上电解锁静音（SD = HIGH）

    /* --- 创建 I2S1 发送通道 --- */
    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_1, I2S_ROLE_MASTER);
    chan_cfg.auto_clear = true;
    chan_cfg.dma_frame_num = 2400;  /* 增大 DMA 缓冲：2400帧 × 2B = 4800 B */
    chan_cfg.dma_desc_num  = 6;     /* 6 个描述符，总能存下 16KB+ 的 WAV */
    ESP_ERROR_CHECK(i2s_new_channel(&chan_cfg, &s_tx_chan, NULL));

    /* --- 配置标准 I2S 模式 --- */
    i2s_std_config_t std_cfg = {
        .clk_cfg = {
            .sample_rate_hz = SAMPLE_RATE,
            .clk_src = I2S_CLK_SRC_DEFAULT,
            .mclk_multiple = I2S_MCLK_MULTIPLE_256,
        },
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_BITS, I2S_SLOT_MODE_MONO),
        .gpio_cfg = {
            .mclk = I2S_GPIO_UNUSED,    // 不启用 MCLK
            .bclk = I2S1_BCLK,
            .ws   = I2S1_LRCLK,
            .dout = I2S1_DOUT,
            .din  = I2S_GPIO_UNUSED,
            .invert_flags = {
                .mclk_inv = false,
                .bclk_inv = false,
                .ws_inv   = false,
            },
        },
    };
    ESP_ERROR_CHECK(i2s_channel_init_std_mode(s_tx_chan, &std_cfg));
    ESP_ERROR_CHECK(i2s_channel_enable(s_tx_chan));

    ESP_LOGI(TAG, "I2S1 initialized: %d Hz mono, BCLK=%d LRCLK=%d DOUT=%d SD=%d",
             SAMPLE_RATE, I2S1_BCLK, I2S1_LRCLK, I2S1_DOUT, SD_MUTE_PIN);
}

/* ================================================================== */
/*  静音控制                                                           */
/* ================================================================== */
void audio_set_mute(bool mute_en)
{
    int level = mute_en ? 0 : 1;   // LOW=静音, HIGH=播放
    gpio_set_level(SD_MUTE_PIN, level);
    ESP_LOGD(TAG, "Mute %s", mute_en ? "ON" : "OFF");
}

/* ================================================================== */
/*  WAV 解析与播放                                                      */
/* ================================================================== */
bool audio_play_wav(const uint8_t *wav_buf, size_t buf_len)
{
    if (!wav_buf || buf_len < sizeof(wav_header_t) + sizeof(wav_data_chunk_t)) {
        ESP_LOGE(TAG, "Invalid WAV buffer");
        return false;
    }

    /* 解析 WAV 头 */
    const wav_header_t *hdr = (const wav_header_t *)wav_buf;

    if (memcmp(hdr->riff_id, "RIFF", 4) != 0 ||
        memcmp(hdr->wave_id, "WAVE", 4) != 0 ||
        memcmp(hdr->fmt_id, "fmt ", 4) != 0) {
        ESP_LOGE(TAG, "Not a valid WAV file");
        return false;
    }

    if (hdr->audio_format != 1) {
        ESP_LOGE(TAG, "Only PCM format supported, got format=%d", hdr->audio_format);
        return false;
    }

    /* 跳过 fmt chunk 后找到 data chunk */
    size_t offset = sizeof(wav_header_t);
    if (hdr->fmt_size > 16) {
        offset += hdr->fmt_size - 16;  // 跳过扩展 fmt 字段
    }

    /* 搜索 "data" chunk */
    const wav_data_chunk_t *data_chunk = NULL;
    while (offset + sizeof(wav_data_chunk_t) <= buf_len) {
        const wav_data_chunk_t *chunk = (const wav_data_chunk_t *)(wav_buf + offset);
        if (memcmp(chunk->data_id, "data", 4) == 0) {
            data_chunk = chunk;
            break;
        }
        offset += sizeof(wav_data_chunk_t) + chunk->data_size;
    }

    if (!data_chunk) {
        ESP_LOGE(TAG, "No 'data' chunk found in WAV");
        return false;
    }

    const uint8_t *pcm_data = (const uint8_t *)(data_chunk + 1);
    size_t pcm_len = data_chunk->data_size;

    if (pcm_data + pcm_len > wav_buf + buf_len) {
        ESP_LOGE(TAG, "WAV data chunk exceeds buffer");
        return false;
    }

    ESP_LOGI(TAG, "WAV: %u Hz, %u ch, %u bits, PCM=%u bytes",
             (unsigned int)hdr->sample_rate,
             (unsigned int)hdr->channels,
             (unsigned int)hdr->bits_per_sample,
             (unsigned int)pcm_len);

    /* 写入全部 PCM 数据到 I2S TX DMA */
    size_t bytes_written = 0;
    esp_err_t ret = i2s_channel_write(s_tx_chan, pcm_data, pcm_len,
                                       &bytes_written, portMAX_DELAY);
    if (ret == ESP_ERR_TIMEOUT) {
        // DMA 缓冲区满，稍后重试
        ESP_LOGW(TAG, "I2S TX timeout, DMA may be full");
        return false;
    }
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "I2S write failed: %s", esp_err_to_name(ret));
        return false;
    }

    s_pcm_bytes = pcm_len;
    s_playing = true;
    ESP_LOGI(TAG, "WAV playback started: %u bytes written, %u ms",
             (unsigned int)bytes_written,
             (unsigned int)(pcm_len * 1000 / hdr->byte_rate));
    return true;
}

/* ================================================================== */
/*  等待播放完成                                                        */
/* ================================================================== */
void audio_wait_play_finish(void)
{
    if (!s_playing) return;

    // 计算理论播放总时长：
    //   16 bit 单声道 @ 16000 Hz → 32000 bytes/s
    //   duration_ms = pcm_bytes * 1000 / 32000
    //   加 20 ms 余量覆盖 DMA FIFO 排空和中断延迟
    uint32_t duration_ms = (uint32_t)(s_pcm_bytes * 1000 / 32000) + 20;

    vTaskDelay(pdMS_TO_TICKS(duration_ms));

    s_playing = false;
    ESP_LOGI(TAG, "Playback finished (%lu ms)", (unsigned long)duration_ms);
}

/* ================================================================== */
/*  内置测试 WAV — 上电验证喇叭                                         */
/* ================================================================== */

/**
 * 440 Hz 正弦波 Beep，持续 100 ms，PCM 16 bit 单声道 16000 Hz。
 *
 * 使用 numpy 生成：
 *   import numpy as np
 *   t = np.arange(0, 0.1, 1/16000)
 *   beep = (np.sin(2 * np.pi * 440 * t) * 16000).astype(np.int16)
 */
static const int16_t test_beep_pcm[] = {
#include "test_beep_pcm.inc"
};

#define TEST_BEEP_PCM_BYTES  sizeof(test_beep_pcm)

/**
 * 在 PCM 数据前拼接一个最小 WAV 头，生成可被 audio_play_wav 解析的内存 WAV。
 */
static void build_wav_header(uint8_t *buf,
                              uint32_t pcm_size,
                              uint16_t channels,
                              uint32_t sample_rate,
                              uint16_t bits_per_sample)
{
    uint32_t riff_size = 36 + pcm_size;
    uint16_t block_align = channels * (bits_per_sample / 8);
    uint32_t byte_rate = sample_rate * block_align;

    // RIFF header
    memcpy(buf, "RIFF", 4);
    buf[4] = riff_size & 0xFF; buf[5] = (riff_size >> 8) & 0xFF;
    buf[6] = (riff_size >> 16) & 0xFF; buf[7] = (riff_size >> 24) & 0xFF;
    memcpy(buf + 8, "WAVE", 4);

    // fmt chunk
    memcpy(buf + 12, "fmt ", 4);
    buf[16] = 16; buf[17] = 0; buf[18] = 0; buf[19] = 0;   // fmt_size = 16
    buf[20] = 1;  buf[21] = 0;                              // PCM = 1
    buf[22] = channels & 0xFF; buf[23] = (channels >> 8) & 0xFF;
    buf[24] = sample_rate & 0xFF; buf[25] = (sample_rate >> 8) & 0xFF;
    buf[26] = (sample_rate >> 16) & 0xFF; buf[27] = (sample_rate >> 24) & 0xFF;
    buf[28] = byte_rate & 0xFF; buf[29] = (byte_rate >> 8) & 0xFF;
    buf[30] = (byte_rate >> 16) & 0xFF; buf[31] = (byte_rate >> 24) & 0xFF;
    buf[32] = block_align & 0xFF; buf[33] = (block_align >> 8) & 0xFF;
    buf[34] = bits_per_sample & 0xFF; buf[35] = (bits_per_sample >> 8) & 0xFF;

    // data chunk
    memcpy(buf + 36, "data", 4);
    buf[40] = pcm_size & 0xFF; buf[41] = (pcm_size >> 8) & 0xFF;
    buf[42] = (pcm_size >> 16) & 0xFF; buf[43] = (pcm_size >> 24) & 0xFF;
}

/** 上电自检：播放内置 beep WAV，验证喇叭连接正常。 */
void audio_player_self_test(void)
{
    ESP_LOGI(TAG, "=== Self-test: playing 440 Hz beep ===");

    // 构造内存 WAV
    #define WAV_HEADER_SIZE 44
    uint8_t wav_buf[WAV_HEADER_SIZE + TEST_BEEP_PCM_BYTES];
    build_wav_header(wav_buf, TEST_BEEP_PCM_BYTES, 1, SAMPLE_RATE, 16);
    memcpy(wav_buf + WAV_HEADER_SIZE, test_beep_pcm, TEST_BEEP_PCM_BYTES);

    audio_set_mute(false);  // 解除静音

    if (audio_play_wav(wav_buf, sizeof(wav_buf))) {
        audio_wait_play_finish();
        ESP_LOGI(TAG, "Self-test PASSED");
    } else {
        ESP_LOGE(TAG, "Self-test FAILED");
    }
}

/* ================================================================== */
/*  合成语音 "你好"                                                    */
/* ================================================================== */

/**
 * @brief 包络 + 频率变化的语音片段生成器。
 *
 * @param buf        输出缓冲区。
 * @param count      采样点数。
 * @param freq_hz_fn 回调: 传入进度 [0,1], 返回该点频率(Hz)。
 * @param amp_fn     回调: 传入进度 [0,1], 返回振幅系数 [0,1]。
 * @param sample_rate 采样率。
 */
typedef float (*freq_fn_t)(float progress);
typedef float (*amp_fn_t)(float progress);

static void synth_syllable(int16_t *buf, int count,
                            freq_fn_t freq_fn,
                            amp_fn_t amp_fn,
                            int sample_rate)
{
    float phase = 0.0f;

    for (int i = 0; i < count; i++) {
        float progress = (float)i / count;
        float freq = freq_fn(progress);
        float amp   = amp_fn(progress);

        /* 相位积分累积避免频率突变导致的爆音 */
        phase += freq / sample_rate;
        if (phase > 1.0f) phase -= 1.0f;

        buf[i] = (int16_t)(amp * 16000.0f * sinf(2.0f * (float)M_PI * phase));
    }
}

/* ========== "nǐ" (三声): 低→降→升 音调曲线 ========== */

static float freq_ni(float p) {
    /* 三声(214): 从半低(200Hz)下降到底(140Hz)再升到中(280Hz)
     *   p∈[0,0.2]: 保持在 ~200 Hz
     *   p∈[0.2,0.5]: 降到 ~120 Hz
     *   p∈[0.5,1.0]: 升到 ~280 Hz
     */
    if (p < 0.2f)      return 200.0f;
    if (p < 0.5f)      return 200.0f + (120.0f - 200.0f) * (p - 0.2f) / 0.3f;
    else                return 120.0f + (280.0f - 120.0f) * (p - 0.5f) / 0.5f;
}

static float amp_ni(float p) {
    /* 渐入 8%, 渐出 12% */
    if (p < 0.08f)      return p / 0.08f;
    if (p > 0.88f)      return (1.0f - p) / 0.12f;
    return 1.0f;
}

/* ========== "hǎo" (三声): 降→升 ========== */

static float freq_hao(float p) {
    /* 三声: 从中高(350Hz)降到(180Hz)再升到(300Hz) */
    if (p < 0.4f)      return 350.0f + (180.0f - 350.0f) * p / 0.4f;
    else                return 180.0f + (300.0f - 180.0f) * (p - 0.4f) / 0.6f;
}

static float amp_hao(float p) {
    if (p < 0.08f)      return p / 0.08f;
    if (p > 0.88f)      return (1.0f - p) / 0.12f;
    return 1.0f;
}

void audio_play_hello(void)
{
    ESP_LOGI(TAG, "=== Playing synthesized 'ni hao' (improved) ===");

    const int sr       = SAMPLE_RATE;

    /* 音节 1 "nǐ": 三声降升调, 320 ms (原200太短) */
    #define N_S1   (sr * 320 / 1000)
    /* 间隙: 30 ms 静音 */
    #define N_GAP  (sr * 30 / 1000)
    /* 音节 2 "hǎo": 三声降升调, 380 ms */
    #define N_S2   (sr * 380 / 1000)
    #define N_TOT  (N_S1 + N_GAP + N_S2)

    size_t pcm_bytes = N_TOT * sizeof(int16_t);
    size_t wav_total = 44 + pcm_bytes;

    uint8_t *wav_buf = (uint8_t *)malloc(wav_total);
    if (!wav_buf) {
        ESP_LOGE(TAG, "Failed to alloc WAV buffer");
        return;
    }

    int16_t *pcm = (int16_t *)(wav_buf + 44);

    /* 音节 1 */
    synth_syllable(pcm, N_S1, freq_ni, amp_ni, sr);

    /* 间隙 */
    for (int i = 0; i < N_GAP; i++) pcm[N_S1 + i] = 0;

    /* 音节 2 */
    synth_syllable(pcm + N_S1 + N_GAP, N_S2, freq_hao, amp_hao, sr);

    /* WAV 头 + 播放 */
    build_wav_header(wav_buf, pcm_bytes, 1, sr, 16);
    audio_set_mute(false);

    if (audio_play_wav(wav_buf, wav_total)) {
        audio_wait_play_finish();
        ESP_LOGI(TAG, "'ni hao' done (%d ms)", 320 + 30 + 380);
    } else {
        ESP_LOGE(TAG, "'ni hao' failed");
    }

    free(wav_buf);
}
