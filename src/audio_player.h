#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 初始化 I2S1 外设驱动 MAX98357A 数字功放。
 *
 * 引脚配置：
 *   BCLK  = GPIO15
 *   LRCLK = GPIO16
 *   DOUT  = GPIO17
 *   SD    = GPIO18（静音控制，高电平播放 / 低电平静音）
 *
 * 音频参数：16 bit 单声道、16000 Hz、Philips I2S 模式、无 MCLK。
 *
 * @note 调用后默认解除静音（SD = HIGH）。
 */
void audio_player_init(void);

/**
 * @brief 控制 MAX98357A 静音状态。
 *
 * @param mute_en  true = 静音（SD = LOW），false = 播放（SD = HIGH）。
 */
void audio_set_mute(bool mute_en);

/**
 * @brief 播放内存中的 WAV 音频（PCM, 16 bit, 单声道）。
 *
 * 函数立即返回，不阻塞。播放完成后可通过
 * audio_wait_play_finish() 等待。
 *
 * @param wav_buf  指向完整 WAV 文件数据（含 RIFF 头）。
 * @param buf_len  WAV 数据字节数。
 * @return true  成功启动播放。
 * @return false 参数无效或 I2S 忙。
 */
bool audio_play_wav(const uint8_t *wav_buf, size_t buf_len);

/**
 * @brief 直接播放 PCM 数据（16 bit 单声道，16000 Hz）。
 *
 * 直接写入 I2S DMA，无需 WAV 头，适用于 TTS 流式输出。
 *
 * @param pcm_data     PCM 数据指针。
 * @param sample_count 采样点数。
 */
void audio_play_pcm(const int16_t *pcm_data, size_t sample_count);

/**
 * @brief 阻塞等待当前播放完成。
 *
 * 若当前无播放任务，立即返回。
 */
void audio_wait_play_finish(void);

/**
 * @brief 上电自检：播放内置 440 Hz beep，验证喇叭连接。
 *
 * 调用 audio_player_init() 之后即可调用。
 */
void audio_player_self_test(void);

/**
 * @brief 播放合成语音"你好"。
 *
 * 使用双频正弦波模拟双音节音调：
 *   "nǐ" → ~280 Hz, 200 ms
 *   "hǎo" → ~350 Hz, 300 ms
 *
 * 函数内部分配临时 WAV 缓冲区、播放、等待完成后释放，全程阻塞约 0.5 秒。
 */
void audio_play_hello(void);

#ifdef __cplusplus
}
#endif
