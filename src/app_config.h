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

