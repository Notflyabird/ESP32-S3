#pragma once

#include <stddef.h>
#include <stdint.h>

#include "app_config.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ====================== 采样率配置 ====================== */
/* 见 app_config.h: DSP_INPUT_RATE / DSP_OUTPUT_RATE / DSP_UPSAMPLE_L */

/* ====================== DC 阻塞滤波器 ====================== */
/* 一阶 IIR 高通: y[n] = α·y[n-1] + x[n] − x[n-1], fc ≈ 40Hz @16kHz */
typedef struct {
    float y_prev;
    float x_prev;
} dc_blocker_t;

/* ====================== AGC 动态增益 ====================== */
/* RMS 反馈式 + 攻放时间常数平滑，替代固定增益 */
typedef struct {
    float s_prev;   /* 指数窗 RMS² 状态 */
    float g_prev;   /* 平滑后增益 */
} agc_t;

/* ====================== 3 倍多相上采样 ====================== */
/* 33 抽头 FIR (3 相 × 11 抽头), 16k→48k, Hamming 窗, fc=6kHz */
#define UPSAMPLE_TAPS_PER_PHASE  11

typedef struct {
    float delay[UPSAMPLE_TAPS_PER_PHASE];   /* delay[0]=最新样本 */
} upsample3x_t;

/* ====================== DSP 处理链 ====================== */
/* 顺序: DC 阻塞 → AGC → 上采样
 * (tanh 限幅 / mono→stereo / I2S 写入由 audio_player.c 在 48kHz 域完成) */
typedef struct {
    dc_blocker_t  dc;
    agc_t         agc;
    upsample3x_t  ups;
} dsp_chain_t;

/**
 * @brief 重置 DSP 链状态（句首调用，避免增益漂移与瞬变）。
 */
void dsp_chain_reset(dsp_chain_t *st);

/**
 * @brief 处理一段 16kHz mono int16 → 48kHz float。
 *
 * 完整链: DC 阻塞 → AGC → 3× 上采样。
 * 每个输入样本产出 DSP_UPSAMPLE_L 个 float 输出样本。
 *
 * @param st    DSP 链状态。
 * @param in    输入 16kHz mono PCM (int16)。
 * @param n_in  输入样本数。
 * @param out   输出缓冲，长度 = n_in * DSP_UPSAMPLE_L。
 */
void dsp_chain_process(dsp_chain_t *st,
                       const int16_t *in, size_t n_in,
                       float *out);

#ifdef __cplusplus
}
#endif
