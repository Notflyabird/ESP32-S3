#include "audio_input.h"

#include <limits.h>
#include <stdlib.h>

#include "app_config.h"
#include "driver/i2s_std.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"

/* I2S0 RX channel handle */
static i2s_chan_handle_t s_rx_chan = NULL;

void audio_input_init(void)
{
    /* 创建 I2S0 接收通道 */
    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(APP_I2S_PORT, I2S_ROLE_MASTER);
    chan_cfg.auto_clear = true;
    ESP_ERROR_CHECK(i2s_new_channel(&chan_cfg, NULL, &s_rx_chan));

    /* 配置标准 I2S 接收模式 */
    i2s_std_config_t std_cfg = {
        .clk_cfg = {
            .sample_rate_hz = APP_SAMPLE_RATE,
            .clk_src = I2S_CLK_SRC_PLL_160M,  /* 160 MHz PLL: better precision */
            .mclk_multiple = I2S_MCLK_MULTIPLE_256,
        },
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(
                I2S_DATA_BIT_WIDTH_32BIT, I2S_SLOT_MODE_MONO),
        .gpio_cfg = {
            .mclk = I2S_GPIO_UNUSED,
            .bclk = APP_PIN_BCLK,
            .ws   = APP_PIN_WS,
            .dout = I2S_GPIO_UNUSED,
            .din  = APP_PIN_SD,
            .invert_flags = {
                .mclk_inv = false,
                .bclk_inv = false,
                .ws_inv   = false,
            },
        },
    };
    /* INMP441 L/R 接 GND → 左声道，显式指定 LEFT 时隙，避免默认值不确定 */
    std_cfg.slot_cfg.slot_mask = I2S_STD_SLOT_LEFT;
    ESP_ERROR_CHECK(i2s_channel_init_std_mode(s_rx_chan, &std_cfg));
    ESP_ERROR_CHECK(i2s_channel_enable(s_rx_chan));
    ESP_LOGI("AUDIO_IN", "I2S0 INMP441 ready: 16kHz 32bit mono, BCLK=%d WS=%d SD=%d",
             APP_PIN_BCLK, APP_PIN_WS, APP_PIN_SD);
}

void *audio_input_alloc(size_t size)
{
    void *buffer = heap_caps_malloc(size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    return buffer != NULL ? buffer : malloc(size);
}

static int16_t sample_to_pcm16(int32_t sample)
{
    sample >>= APP_MIC_SAMPLE_SHIFT;
    if (sample > INT16_MAX) {
        return INT16_MAX;
    }
    if (sample < INT16_MIN) {
        return INT16_MIN;
    }
    return (int16_t)sample;
}

esp_err_t audio_input_read_pcm_chunk(int32_t *raw, int16_t *pcm, int sample_count)
{
    const size_t wanted = (size_t)sample_count * sizeof(*raw);
    size_t total = 0;

    while (total < wanted) {
        size_t bytes_read = 0;
        esp_err_t err = i2s_channel_read(s_rx_chan,
                                         (uint8_t *)raw + total,
                                         wanted - total,
                                         &bytes_read, portMAX_DELAY);
        if (err != ESP_OK) {
            return err;
        }
        if (bytes_read == 0) {
            return ESP_ERR_INVALID_SIZE;
        }
        total += bytes_read;
    }

    for (int i = 0; i < sample_count; ++i) {
        pcm[i] = sample_to_pcm16(raw[i]);
    }
    return ESP_OK;
}

