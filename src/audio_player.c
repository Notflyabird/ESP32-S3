#include "audio_player.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "app_config.h"
#include "audio_dsp.h"
#include "driver/gpio.h"
#include "driver/i2s_std.h"
#include "esp_check.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "AUDIO_PLAY";

/* ====================== 引脚定义（与 app_config.h SPEAKER_PIN_* 一致）====================== */
#define I2S1_BCLK   SPEAKER_PIN_BCLK
#define I2S1_LRCLK  SPEAKER_PIN_LRCLK
#define I2S1_DOUT   SPEAKER_PIN_DOUT
#define SD_MUTE_PIN SPEAKER_PIN_SD

/* ====================== 音频参数 ====================== */
/* I2S 输出 48kHz stereo（DSP 上采样后）；输入域 16kHz 见 DSP_INPUT_RATE */
#define I2S_BITS            I2S_DATA_BIT_WIDTH_16BIT
/* STEREO 模式：MAX98357A 是立体声功放，双声道填充确保数据连续 */
#define PLAYBACK_CHANNELS   2
/* 渲染分块：每次处理 2048 个 16kHz 输入样本 → 6144 个 48kHz 输出样本 */
#define RENDER_INPUT_CHUNK  2048

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
static size_t s_pcm_bytes = 0;  /* 已写入 I2S 的 48kHz stereo 字节数（用于时长计算） */

/* ---------- I2S1 通道句柄 ---------- */
static i2s_chan_handle_t s_tx_chan = NULL;

/* ---------- DSP 链 + PSRAM 输出缓冲 ---------- */
static dsp_chain_t s_dsp;                /* DC 阻塞 / AGC / 上采样 状态 */
static float    *s_dsp_out    = NULL;    /* DSP 输出 float[RENDER_INPUT_CHUNK*L] = 24KB PSRAM */
static int16_t  *s_out_stereo = NULL;    /* stereo int16[RENDER_INPUT_CHUNK*L*2] = 24KB PSRAM */

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
    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(SPEAKER_I2S_PORT, I2S_ROLE_MASTER);
    chan_cfg.auto_clear = true;
    chan_cfg.dma_frame_num = 1024;  /* 1024 帧 × 4B(stereo) = 4096B */
    chan_cfg.dma_desc_num  = 6;     /* 6 描述符 = 24KB ≈ 128ms 缓冲 @48kHz stereo */
    ESP_ERROR_CHECK(i2s_new_channel(&chan_cfg, &s_tx_chan, NULL));

    /* --- 配置标准 I2S 模式（STEREO，48kHz：DSP 上采样后输出） --- */
    i2s_std_config_t std_cfg = {
        .clk_cfg = {
            .sample_rate_hz = DSP_OUTPUT_RATE,
            .clk_src = I2S_CLK_SRC_PLL_160M,
            .mclk_multiple = I2S_MCLK_MULTIPLE_256,
        },
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_BITS, I2S_SLOT_MODE_STEREO),
        .gpio_cfg = {
            .mclk = I2S_GPIO_UNUSED,
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

    /* --- 分配 PSRAM 输出缓冲（DMA 用户缓冲，驱动会拷贝到内部 DMA buf） --- */
    size_t dsp_out_bytes   = RENDER_INPUT_CHUNK * DSP_UPSAMPLE_L * sizeof(float);
    size_t stereo_buf_bytes = RENDER_INPUT_CHUNK * DSP_UPSAMPLE_L * PLAYBACK_CHANNELS * sizeof(int16_t);
    s_dsp_out = (float *)heap_caps_malloc(dsp_out_bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    s_out_stereo = (int16_t *)heap_caps_malloc(stereo_buf_bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (s_dsp_out == NULL) {
        s_dsp_out = (float *)malloc(dsp_out_bytes);  /* 回退内部 RAM */
    }
    if (s_out_stereo == NULL) {
        s_out_stereo = (int16_t *)malloc(stereo_buf_bytes);
    }
    ESP_ERROR_CHECK(s_dsp_out == NULL || s_out_stereo == NULL ? ESP_ERR_NO_MEM : ESP_OK);

    /* --- 初始化 DSP 链 --- */
    dsp_chain_reset(&s_dsp);

    ESP_LOGI(TAG, "I2S1 initialized: %d Hz stereo, BCLK=%d LRCLK=%d DOUT=%d SD=%d",
             DSP_OUTPUT_RATE, I2S1_BCLK, I2S1_LRCLK, I2S1_DOUT, SD_MUTE_PIN);
    ESP_LOGI(TAG, "DSP chain ready: %d->%d Hz (L=%d), DC block + AGC + upsampling",
             DSP_INPUT_RATE, DSP_OUTPUT_RATE, DSP_UPSAMPLE_L);
}

/* ================================================================== */
/*  静音控制（渐变：避免功放突然开/关产生"啪"爆音）                    */
/* ================================================================== */
void audio_set_mute(bool mute_en)
{
    if (!mute_en) {
        /* 解除静音：先拉高 SD，等待功放稳定再播放 */
        gpio_set_level(SD_MUTE_PIN, 1);
        vTaskDelay(pdMS_TO_TICKS(10));
    } else {
        /* 静音：等待最后的数据播完，再拉低 SD */
        vTaskDelay(pdMS_TO_TICKS(5));
        gpio_set_level(SD_MUTE_PIN, 0);
    }
}

/* ================================================================== */
/*  DSP 链重置（句首调用）                                              */
/* ================================================================== */
void audio_player_reset_dsp(void)
{
    dsp_chain_reset(&s_dsp);
    s_pcm_bytes = 0;   /* 句首清零播放统计，供 audio_wait_play_finish 计算时长 */
}

/* ================================================================== */
/*  核心渲染：16kHz mono int16 → 48kHz stereo I2S                      */
/*  完整链: DC 阻塞 → AGC → 3× 上采样 → tanh 软限幅 → mono→stereo      */
/*  → i2s_channel_write（复用 PSRAM 缓冲 s_out_stereo）               */
/* ================================================================== */
static void audio_render_mono(const int16_t *mono, size_t sample_count)
{
    size_t offset = 0;
    while (offset < sample_count) {
        size_t chunk = sample_count - offset;
        if (chunk > RENDER_INPUT_CHUNK) chunk = RENDER_INPUT_CHUNK;

        /* ①②③ DC 阻塞 → AGC → 3× 上采样: 16kHz int16 → 48kHz float */
        dsp_chain_process(&s_dsp, mono + offset, chunk, s_dsp_out);

        /* ④ tanh 软限幅 + 饱和 int16 + ⑤ mono→stereo */
        size_t out_samples = chunk * DSP_UPSAMPLE_L;
        for (size_t i = 0; i < out_samples; ++i) {
            /* tanh 在 48kHz 域限幅，避免 16kHz 域谐波混叠回可听频带 */
            float fv = s_dsp_out[i] / 32768.0f;
            fv = tanhf(fv) * 32767.0f;
            int16_t s = (int16_t)fv;
            s_out_stereo[i * 2]     = s;  /* L */
            s_out_stereo[i * 2 + 1] = s;  /* R */
        }

        /* ⑥ 写入 I2S（阻塞，自动等待 DMA 有空位） */
        size_t bytes_to_write = out_samples * PLAYBACK_CHANNELS * sizeof(int16_t);
        size_t bytes_written = 0;
        esp_err_t ret = i2s_channel_write(s_tx_chan, s_out_stereo,
                                           bytes_to_write, &bytes_written, portMAX_DELAY);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "I2S write failed: %s", esp_err_to_name(ret));
            return;
        }
        s_pcm_bytes += bytes_written;
        offset += chunk;
    }
    s_playing = true;
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

    /* 校验格式：仅支持 mono 16bit PCM（输入域 16kHz，DSP 上采样到 48kHz 输出）*/
    if (hdr->channels != 1 || hdr->bits_per_sample != 16) {
        ESP_LOGE(TAG, "Unsupported WAV: %u ch, %u bits (expect 1 ch, 16 bits)",
                 (unsigned int)hdr->channels, (unsigned int)hdr->bits_per_sample);
        return false;
    }

    size_t num_samples = pcm_len / sizeof(int16_t);
    if (num_samples == 0) {
        ESP_LOGE(TAG, "Empty WAV data");
        return false;
    }

    /* 句首重置 DSP 链 + 清零播放统计（WAV 独立播放，每次都是新句）*/
    audio_player_reset_dsp();

    /* 渲染播放：16kHz mono → DC 阻塞 → AGC → 上采样 → tanh → stereo → I2S */
    audio_render_mono((const int16_t *)pcm_data, num_samples);
    return s_playing;
}

/* ================================================================== */
/*  PCM 直接播放（TTS 流式输出）                                        */
/*  完整 DSP 链由 audio_render_mono 统一处理：                          */
/*  DC 阻塞 → AGC → 3× 上采样 → tanh → mono→stereo → I2S              */
/*  注：流式播放不重置 DSP（跨 chunk 维持状态），句首由                  */
/*  tts_player 调用 audio_player_reset_dsp() 重置。                    */
/* ================================================================== */
void audio_play_pcm(const int16_t *pcm_data, size_t sample_count)
{
    if (!pcm_data || sample_count == 0) return;
    audio_render_mono(pcm_data, sample_count);
}

/* ================================================================== */
/*  等待播放完成                                                        */
/* ================================================================== */
void audio_wait_play_finish(void)
{
    if (!s_playing) return;

    /* 计算理论播放总时长：
     *   16 bit stereo @ 48000 Hz → 192000 bytes/s
     *   duration_ms = pcm_bytes * 1000 / 192000
     *   加 20 ms 余量覆盖 DMA FIFO 排空和中断延迟
     */
    uint32_t duration_ms = (uint32_t)(s_pcm_bytes * 1000 / (DSP_OUTPUT_RATE * PLAYBACK_CHANNELS * 2)) + 20;

    vTaskDelay(pdMS_TO_TICKS(duration_ms));

    s_playing = false;
    ESP_LOGD(TAG, "Playback finished (%lu ms)", (unsigned long)duration_ms);
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
    // 构造内存 WAV
    #define WAV_HEADER_SIZE 44
    uint8_t wav_buf[WAV_HEADER_SIZE + TEST_BEEP_PCM_BYTES];
    build_wav_header(wav_buf, TEST_BEEP_PCM_BYTES, 1, DSP_INPUT_RATE, 16);
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
    ESP_LOGI(TAG, "Playing 'ni hao'");

    const int sr       = DSP_INPUT_RATE;

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
    } else {
        ESP_LOGE(TAG, "'ni hao' failed");
    }

    free(wav_buf);
}
