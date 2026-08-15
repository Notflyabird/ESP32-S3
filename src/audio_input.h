#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

#include "esp_err.h"

void audio_input_init(void);
void *audio_input_alloc(size_t size);
esp_err_t audio_input_read_pcm_chunk(int32_t *raw, int16_t *pcm, int sample_count);

/**
 * @brief 暂停 I2S0 RX 通道 DMA（L6-A Light-sleep 前调用）。
 *        仅 disable 通道，不销毁 handle，之后可 audio_input_resume() 重启。
 * @return ESP_OK / i2s 错误码。
 */
esp_err_t audio_input_stop(void);

/** @brief 恢复（重新 enable）I2S0 RX 通道 DMA（L6-A Light-sleep 唤醒后调用）。*/
esp_err_t audio_input_resume(void);

