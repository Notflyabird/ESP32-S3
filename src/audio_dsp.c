#include "audio_dsp.h"

#include <math.h>
#include <stdbool.h>
#include <string.h>

#define PI_F  3.14159265358979323846f

/* ================================================================== */
/*  3 倍多相 FIR 上采样系数 (33 抽头 = 3 相 × 11 抽头)                 */
/*  Hamming 窗, fc=6kHz, DC 增益 = L = 3                              */
/*  生成: h[n]=sinc(2*fc*(n-M)/Fs)*(2*fc/Fs)*hamming(N)*L, N=33, M=16  */
/* ================================================================== */
static const float g0[UPSAMPLE_TAPS_PER_PHASE] = {
     0.00000f, -0.00819f,  0.03477f, -0.06075f,  0.00000f,
     0.66927f,  0.46074f, -0.10743f,  0.00000f,  0.01746f, -0.00786f
};
static const float g1[UPSAMPLE_TAPS_PER_PHASE] = {
    -0.00399f,  0.00000f,  0.03378f, -0.11394f,  0.20766f,
     0.75000f,  0.20766f, -0.11394f,  0.03378f,  0.00000f, -0.00399f
};
static const float g2[UPSAMPLE_TAPS_PER_PHASE] = {
    -0.00786f,  0.01746f,  0.00000f, -0.10743f,  0.46074f,
     0.66927f,  0.00000f, -0.06075f,  0.03477f, -0.00819f,  0.00000f
};

/* ================================================================== */
/*  从 app_config.h 宏推导的系数（首次调用时计算并缓存）              */
/*  DC:  α = 1 − 2π·fc·Ts                                             */
/*  AGC: λ = exp(−Ts/τ_win),  α = 1 − exp(−Ts/τ)                      */
/* ================================================================== */
static float s_dc_alpha;
static float s_agc_lambda;
static float s_agc_one_m_lambda;
static float s_agc_alpha_atk;
static float s_agc_alpha_rel;
static float s_agc_s_init;   /* 句首 RMS² 初值，使 g_target≈AGC_GAIN_INIT */
static bool  s_coeff_ready = false;

static void ensure_coefficients(void)
{
    if (s_coeff_ready) return;

    const float Ts = 1.0f / (float)DSP_INPUT_RATE;

    /* DC 阻塞: fc ≈ (1−α)·Fs / (2π) */
    s_dc_alpha = 1.0f - 2.0f * PI_F * (float)DC_BLOCK_FC_HZ * Ts;

    /* AGC 指数窗与攻放时间常数 */
    s_agc_lambda       = expf(-Ts / ((float)AGC_TAU_WIN_MS * 1e-3f));
    s_agc_one_m_lambda = 1.0f - s_agc_lambda;
    s_agc_alpha_atk    = 1.0f - expf(-Ts / ((float)AGC_TAU_ATK_MS * 1e-3f));
    s_agc_alpha_rel    = 1.0f - expf(-Ts / ((float)AGC_TAU_REL_MS * 1e-3f));

    /* 句首 s_prev 使首样本 g_target = target/g_init ≈ AGC_GAIN_INIT，无瞬变 */
    s_agc_s_init = (AGC_TARGET_RMS / AGC_GAIN_INIT) *
                   (AGC_TARGET_RMS / AGC_GAIN_INIT);

    s_coeff_ready = true;
}

/* ================================================================== */
/*  DSP 链重置（句首调用）                                            */
/* ================================================================== */
void dsp_chain_reset(dsp_chain_t *st)
{
    ensure_coefficients();

    /* DC 阻塞: 初值 0 */
    st->dc.y_prev = 0.0f;
    st->dc.x_prev = 0.0f;

    /* AGC: 增益初始 = AGC_GAIN_INIT，RMS² 初始使 g_target 与之一致 */
    st->agc.g_prev = AGC_GAIN_INIT;
    st->agc.s_prev = s_agc_s_init;

    /* 上采样延迟线清零 */
    memset(st->ups.delay, 0, sizeof(st->ups.delay));
}

/* ================================================================== */
/*  DSP 链处理: 16kHz int16 → 48kHz float                             */
/*  顺序: DC 阻塞 → AGC → 3× 多相上采样                               */
/*  (tanh 限幅 / mono→stereo 由 audio_player.c 在 48kHz 域完成)       */
/* ================================================================== */
void dsp_chain_process(dsp_chain_t *st,
                       const int16_t *in, size_t n_in,
                       float *out)
{
    ensure_coefficients();

    for (size_t n = 0; n < n_in; ++n) {
        /* ① DC 阻塞 (一阶 IIR 高通) */
        float x = (float)in[n];
        float y_dc = s_dc_alpha * st->dc.y_prev + x - st->dc.x_prev;
        st->dc.y_prev = y_dc;
        st->dc.x_prev = x;

        /* ② AGC (RMS 反馈式 + 攻放平滑) */
        st->agc.s_prev = s_agc_lambda * st->agc.s_prev +
                         s_agc_one_m_lambda * (y_dc * y_dc);
        float rms = sqrtf(st->agc.s_prev);
        float g_target = AGC_TARGET_RMS / (rms + 1.0f);
        if (g_target < AGC_GAIN_MIN) {
            g_target = AGC_GAIN_MIN;
        } else if (g_target > AGC_GAIN_MAX) {
            g_target = AGC_GAIN_MAX;
        }
        /* attack: 信号变大→增益需变小(快速降); release: 信号变小→增益需变大(缓慢升) */
        float alpha = (g_target < st->agc.g_prev) ? s_agc_alpha_atk
                                                   : s_agc_alpha_rel;
        st->agc.g_prev += alpha * (g_target - st->agc.g_prev);
        float y_agc = y_dc * st->agc.g_prev;

        /* ③ 3× 多相上采样: 每输入 1 样本产出 3 个 48kHz 样本 */
        /* 移位延迟线: delay[10..1] = delay[9..0] */
        for (int i = UPSAMPLE_TAPS_PER_PHASE - 1; i > 0; --i) {
            st->ups.delay[i] = st->ups.delay[i - 1];
        }
        st->ups.delay[0] = y_agc;

        /* 3 相 FIR 卷积 */
        float a0 = 0.0f, a1 = 0.0f, a2 = 0.0f;
        for (int i = 0; i < UPSAMPLE_TAPS_PER_PHASE; ++i) {
            float d = st->ups.delay[i];
            a0 += g0[i] * d;
            a1 += g1[i] * d;
            a2 += g2[i] * d;
        }
        out[n * DSP_UPSAMPLE_L + 0] = a0;
        out[n * DSP_UPSAMPLE_L + 1] = a1;
        out[n * DSP_UPSAMPLE_L + 2] = a2;
    }
}
