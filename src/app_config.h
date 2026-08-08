#pragma once

#include "driver/gpio.h"
#include "driver/i2s_std.h"

/* ====================== I2S / Microphone ====================== */
#define APP_I2S_PORT I2S_NUM_0
#define APP_PIN_BCLK GPIO_NUM_4
#define APP_PIN_WS   GPIO_NUM_5
#define APP_PIN_SD   GPIO_NUM_6

#define APP_SAMPLE_RATE       16000
/* INMP441: 24-bit signed in 32-bit slot, MSB-aligned.
   Right shift 16 → 16-bit PCM with ~96dB dynamic range,
   avoid clipping (32768) which breaks WakeNet recognition. */
#define APP_MIC_SAMPLE_SHIFT  16
#define APP_COMMAND_TIMEOUT_MS 6000

/* ====================== LCD ST7789 ============================ */
/* SPI (use GPSPI2 / SPI2_HOST) */
#define LCD_PIN_MOSI      GPIO_NUM_11
#define LCD_PIN_SCLK      GPIO_NUM_12
#define LCD_PIN_CS        GPIO_NUM_10
#define LCD_PIN_DC        GPIO_NUM_9
#define LCD_PIN_RST       GPIO_NUM_8
#define LCD_PIN_BL        GPIO_NUM_7

#define LCD_WIDTH         240
#define LCD_HEIGHT        320
#define LCD_SPI_FREQ_HZ   (40 * 1000 * 1000)  /* 40 MHz */

/* ====================== MAX98357A I2S1 Speaker ================ */
#define SPEAKER_I2S_PORT      I2S_NUM_1
#define SPEAKER_PIN_BCLK      GPIO_NUM_15
#define SPEAKER_PIN_LRCLK     GPIO_NUM_16
#define SPEAKER_PIN_DOUT      GPIO_NUM_17
#define SPEAKER_PIN_SD        GPIO_NUM_18   /* SD 静音引脚 */

/* ====================== 音频 DSP 处理链 ======================== */
/* TTS 输出 16kHz mono → 上采样到 48kHz stereo 输出 MAX98357A */
#define DSP_INPUT_RATE        16000          /* TTS PCM 输入采样率 */
#define DSP_OUTPUT_RATE       48000          /* I2S 输出采样率 */
#define DSP_UPSAMPLE_L        3              /* 上采样因子 (16k→48k) */

/* DC 阻塞滤波器（一阶 IIR 高通，16kHz 域）*/
#define DC_BLOCK_FC_HZ        40             /* 截止频率，低于语音基频 80Hz */

/* AGC 动态增益（RMS 反馈式，16kHz 域）*/
#define AGC_TARGET_RMS        6000.0f        /* 目标 RMS 电平 (int16 域, ≈-14.7dBFS) */
#define AGC_TAU_WIN_MS        30             /* RMS 估计窗口时间常数 */
#define AGC_TAU_ATK_MS        20             /* attack 时间常数（信号变大时快速降增益）*/
#define AGC_TAU_REL_MS        200            /* release 时间常数（信号变小时缓慢升增益）*/
#define AGC_GAIN_MIN          0.5f           /* 最小增益，防过压缩 */
#define AGC_GAIN_MAX          8.0f           /* 最大增益，防噪声放大 */
#define AGC_GAIN_INIT         3.0f           /* 句首初始增益，匹配原 ×3 听感 */

