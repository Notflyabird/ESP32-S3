# MAX98357A I2S 音频优化实现计划

## Context（背景）

当前 MAX98357A 功放驱动（[audio_player.c](file:///d:/03_project/esp32/01_code/esp32_s3_0725/ESP32-S3/src/audio_player.c)）已实现 STEREO 模式、tanh 软限幅、渐变静音，TTS 中文播报基本可用。但仍有 4 个可优化方向：

1. **DMA 输出缓冲占用 8KB 内部 RAM**（`static int16_t stereo_buf[4096]`），esp-sr 模型也驻留内部 RAM，压力较大。
2. **采样率仅 16kHz**，存在量化噪声与采样镜像，且未充分利用 MAX98357A 的 DAC 能力。
3. **无 DC 阻塞**，TTS PCM 可能存在 DC 偏置，造成功放静态功耗与低频隆隆声。
4. **固定 ×3 增益**，不同 TTS 句子音量差异大，小声听不清、大声削波。

本次实施全部 4 项优化，目标：节省内部 RAM、提升音质通透度、消除 DC 偏移、统一响度。

### 关键前提（已验证 sdkconfig）
- CPU 主频 **160MHz**（非 240MHz）→ DSP 预算按 160MHz 核算（实测足够）
- PSRAM Octal 80MHz，`SOC_AHB_GDMA_SUPPORT_PSRAM=y` → 用户缓冲可放 PSRAM
- `SPIRAM_MALLOC_ALWAYSINTERNAL=16384` → ≤16KB 的 malloc 进内部 RAM，大缓冲必须显式 `heap_caps_malloc(MALLOC_CAP_SPIRAM)`

---

## DSP 处理链总览

```
[16kHz int16 输入]
   │
   ▼ ① DC 阻塞  (一阶 IIR 高通, fc=40Hz @16kHz)
   ▼ ② AGC      (RMS 反馈式, 替代固定 ×3 增益)
   ▼ ③ 3× 上采样 (33 抽头多相 FIR, 16k→48k)
   ▼ ④ tanh 软限幅 (48kHz 域, 避免谐波混叠)
   ▼ ⑤ mono→stereo 复制
   ▼ ⑥ i2s_channel_write (PSRAM buf → 内部 DMA buf)
[48kHz stereo I2S 输出]
```

**关键设计决策**：DC 阻塞、AGC 放在 16kHz 域（计算量是 48kHz 的 1/3）；tanh 必须在 48kHz 域——若在 16kHz 做 tanh，其产生的谐波会混叠回可听频带。

**总 CPU 开销**：~4.25M cycles/s，160MHz 下占 **2.7% CPU**，实时性无忧。

---

## 优化 1：DMA 输出缓冲迁移到 PSRAM

### 现状
`audio_play_pcm` 中 `static int16_t stereo_buf[2048*2]` = 8KB 内部 RAM。

### 改动
- 删除静态数组，在 `audio_player_init()` 中一次性分配 PSRAM 缓冲：
  ```c
  #define OUT_BUF_SAMPLES (2048 * DSP_UPSAMPLE_L)   // 6144 单声道输出样本
  static int16_t *s_out_stereo = NULL;              // 6144×2×2 = 24KB, PSRAM
  s_out_stereo = heap_caps_malloc(OUT_BUF_SAMPLES * 2 * sizeof(int16_t),
                                  MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  ```
- 分配失败回退 `malloc` + warning，不阻断。
- 缓冲整个播放期复用，deinit 时 `free`。
- **注意**：I2S 驱动的 DMA 描述符缓冲（24KB）仍由驱动分配在内部 RAM，不可迁 PSRAM。

---

## 优化 2：48kHz 上采样（3 倍多相 FIR 插值）

### 滤波器参数
| 参数 | 值 |
|---|---|
| 输出采样率 | 48000 Hz |
| 插值因子 L | 3 |
| FIR 长度 N | 33（3 相 × 11 抽头） |
| 截止 fc | 6000 Hz |
| 窗 | Hamming |
| DC 增益 | L = 3（补偿插零衰减） |

### FIR 多相系数（浮点，3 相 × 11 抽头）
```
g0 (phase 0, y[3n]  ) = [ 0.00000, -0.00819,  0.03477, -0.06075,  0.00000,
                          0.66927,  0.46074, -0.10743,  0.00000,  0.01746, -0.00786]
g1 (phase 1, y[3n+1]) = [-0.00399,  0.00000,  0.03378, -0.11394,  0.20766,
                          0.75000,  0.20766, -0.11394,  0.03378,  0.00000, -0.00399]
g2 (phase 2, y[3n+2]) = [-0.00786,  0.01746,  0.00000, -0.10743,  0.46074,
                          0.66927,  0.00000, -0.06075,  0.03477, -0.00819,  0.00000]
```
对称性：`g2[i]=g0[10-i]`、`g1[i]=g1[10-i]`（可省 ROM，实现时直接用上表）。

### 多相实现
```c
typedef struct { float delay[11]; } upsample3x_t;   // delay[0]=最新

static inline void upsample3x_process(upsample3x_t *st, float x,
                                       float *y0, float *y1, float *y2) {
    for (int i = 10; i > 0; --i) st->delay[i] = st->delay[i-1];
    st->delay[0] = x;
    float a0=0,a1=0,a2=0;
    for (int i = 0; i < 11; ++i) {
        float d = st->delay[i];
        a0 += g0[i]*d; a1 += g1[i]*d; a2 += g2[i]*d;
    }
    *y0=a0; *y1=a1; *y2=a2;
}
```

### 计算开销
每输入样本 33 次浮点 MAC，16kHz 下 ~1.06M cycles/s ≈ 0.66% CPU。

---

## 优化 3：DC 阻塞滤波器（一阶 IIR 高通）

### 算法
```
y[n] = α·y[n-1] + x[n] − x[n-1]
fc ≈ (1−α)·Fs / (2π)
```

### 参数（16kHz 域，fc=40Hz）
- α = 1 − 2π·40/16000 = **0.98429**
- 初值 y[−1]=0, x[−1]=0

### 实现
```c
typedef struct { float y_prev, x_prev; } dc_blocker_t;
static inline float dc_block(dc_blocker_t *st, float x) {
    float y = 0.98429f * st->y_prev + x - st->x_prev;
    st->y_prev = y; st->x_prev = x;
    return y;
}
```

---

## 优化 4：AGC 动态增益（RMS 反馈式 + 攻放时间常数）

### 与固定增益的关系
**AGC 替代固定 `TTS_GAIN=3`**。AGC 把不同句子 RMS 拉到统一目标电平；tanh 在 48kHz 域作安全峰值限幅器，仅对偶发峰值起作用。

### 算法
```
(1) s[n]   = λ·s[n-1] + (1−λ)·x[n]²          // 指数窗 RMS²
(2) g_target = clamp( target_rms / (sqrt(s)+ε), g_min, g_max )
(3) α_sm = (g_target < g[n-1]) ? α_atk : α_rel
(4) g[n] = g[n-1] + α_sm·(g_target − g[n-1])
(5) y[n] = x[n] · g[n]
```

### 参数（16kHz 域）
| 参数 | 值 | 推导 |
|---|---|---|
| target_rms | 6000 | ≈−14.7dBFS，峰值≈24000 略低于满刻度 |
| τ_win (RMS 窗) | 30 ms | λ = exp(−62.5µs/30ms) = **0.99792** |
| τ_atk (attack) | 20 ms | α_atk = 1−exp(−62.5µs/20ms) = **0.00312** |
| τ_rel (release) | 200 ms | α_rel = 1−exp(−62.5µs/200ms) = **0.000313** |
| g_min / g_max | 0.5 / 8.0 | 防过压缩/噪声放大 |
| g_init（句首） | 3.0 | 匹配原 ×3 起始听感 |
| s_init（句首） | 4.0e6 | 使首样本 g_target≈g_init，无启动瞬变 |

### 跨 chunk 流式
`s_prev`、`g_prev` 作为 static 在 `audio_play_pcm` 调用间持久。**句首重置**：`tts_player` 每句流式循环开始前调用 `audio_player_reset_dsp()`，重置 s/g/DC/上采样状态，避免句间 500ms 静默让增益漂到 g_max。

### 实现
```c
typedef struct { float s_prev, g_prev; } agc_t;
static inline float agc_process(agc_t *st, float x) {
    const float lambda=0.99792f, one_m_l=0.00208f, target=6000.0f, eps=1.0f;
    const float g_min=0.5f, g_max=8.0f, a_atk=0.00312f, a_rel=0.000313f;
    st->s_prev = lambda*st->s_prev + one_m_l*(x*x);
    float gt = target / (sqrtf(st->s_prev) + eps);
    if (gt < g_min) gt = g_min; else if (gt > g_max) gt = g_max;
    float a = (gt < st->g_prev) ? a_atk : a_rel;
    st->g_prev += a * (gt - st->g_prev);
    return x * st->g_prev;
}
```

---

## 文件修改清单

### 新增文件
1. **`src/audio_dsp.h`** — 封装 DSP 链接口
   ```c
   #define DSP_INPUT_RATE   16000
   #define DSP_OUTPUT_RATE  48000
   #define DSP_UPSAMPLE_L   3
   typedef struct { dc_blocker_t dc; agc_t agc; upsample3x_t ups; } dsp_chain_t;
   void dsp_chain_reset(dsp_chain_t *st);
   void dsp_chain_process(dsp_chain_t *st, const int16_t *in, size_t n_in, float *out); // out 长 n_in*3
   ```
2. **`src/audio_dsp.c`** — DC 阻塞 + AGC + 多相上采样 + FIR 系数表 + `dsp_chain_process/reset`

### 修改文件
3. **`src/audio_player.c`**
   - `audio_player_init()`：`sample_rate_hz = DSP_OUTPUT_RATE(48000)`；PSRAM 分配 `s_out_stereo`；初始化 `static dsp_chain_t s_dsp`
   - 新增 `audio_player_reset_dsp()`：调用 `dsp_chain_reset`
   - 新增 `static void audio_render_mono(const int16_t *mono, size_t n)`：完整链 DC→AGC→上采样→tanh→mono→stereo→I2S，复用 `s_out_stereo`
   - `audio_play_pcm()`：改为调用 `audio_render_mono`（流式，不重置 DSP）
   - `audio_play_wav()`：解析得 mono PCM 后先 `audio_player_reset_dsp()` 再分块调用 `audio_render_mono`；删除原自行分配 PSRAM stereo_buf 逻辑
   - `audio_wait_play_finish()`：duration 公式改用 `DSP_OUTPUT_RATE`：`s_pcm_bytes*1000/(48000*2*2)+20`
4. **`src/audio_player.h`** — 新增 `void audio_player_reset_dsp(void);`
5. **`src/tts_player.c`** — `tts_task` 流式循环前（`audio_set_mute(false)` 后、首次 `esp_tts_stream_play` 前）插入 `audio_player_reset_dsp()`
6. **`src/app_config.h`** — 新增可调宏：`DSP_INPUT_RATE`/`DSP_OUTPUT_RATE`/`AGC_TARGET_RMS`/`AGC_TAU_WIN_MS`/`AGC_TAU_ATK_MS`/`AGC_TAU_REL_MS`/`AGC_GAIN_MIN`/`AGC_GAIN_MAX`/`AGC_GAIN_INIT`/`DC_BLOCK_FC_HZ`
7. **`src/CMakeLists.txt`** — `SRCS` 增加 `audio_dsp.c`

### I2S 配置变更
| 项 | 现状 | 新值 | 说明 |
|---|---|---|---|
| `sample_rate_hz` | 16000 | **48000** | 上采样后输出速率 |
| `dma_frame_num` | 1024 | 1024 | 不变 |
| `dma_desc_num` | 6 | 6 | 不变；48k 下 24KB = 128ms 缓冲（原 384ms）。若 underrun 提到 8 |
| 其余 | - | - | clk_src/mclk_multiple/auto_clear 不变 |

---

## 验证方法

### 听感验证（板上）
1. **上采样正确性**：播放 `audio_player_self_test` 的 440Hz beep，音调应不变（非 3 倍速）。播放 `audio_play_hello` 正常。
2. **TTS 音质**：播放短句（"一分"）和长句（"语音斗地主计分系统已启动"），对比优化前后：
   - 响度一致（AGC 生效）
   - 无 DC 爆音（DC 阻塞生效，静音→播放切换无"啪"声）
   - 无金属混叠音（上采样 FIR 生效）
   - 无增益泵感（attack/release 平滑）
3. **连续多句**：播报不同音量文本（如唤醒响应"我在" vs 长句计分提示），响度应拉平。
4. **实时性**：`audio_render_mono` 前后取 `esp_timer_get_time()` 打印每 chunk 耗时，确认 < I2S 写入耗时（无 underrun 啪声）。

### 内存验证
5. 启动后打印 `heap_caps_get_free_size(MALLOC_CAP_INTERNAL)`，确认 PSRAM 迁移后内部 RAM 比上采样版内部 buf 方案多出 ~24KB。

### 回归验证
6. 唤醒词"Hi ESP"仍可唤醒（SR 未受影响，TTS 期间仍正确暂停 SR）。
7. GPIO0 撤销计分语音提示正常。

---

## 风险与对策

| 风险 | 对策 |
|---|---|
| 160MHz 下 esp-sr + DSP 抢占 CPU | DSP 仅 2.7% CPU，余量充足；若实测紧张可升 240MHz（非必需） |
| 上采样后 DMA 仅 128ms，TTS 被抢占致 underrun | TTS 任务优先级 5 已高于 SR；若 underrun 提 `dma_desc_num=8` |
| AGC 句首首音节偏轻 | g_init=3.0 + s_init 匹配，首样本即 ~3× 增益 |
| tanh 在 16kHz 域做会混叠 | 严格按链在 48kHz 域做 tanh |
| WAV/beep 在 48k I2S 上变 3 倍速 | audio_play_wav 统一走 audio_render_mono 上采样 |
| PSRAM buf 分配失败 | 回退 malloc + warning，不阻断 |

---

## 实施顺序

1. 新建 `audio_dsp.h`/`audio_dsp.c`（DC + AGC + 上采样 + 系数表 + dsp_chain 接口）
2. 改 `audio_player.c`：I2S 改 48k + PSRAM buf + `audio_render_mono` + `audio_player_reset_dsp` + 重构 `audio_play_pcm`/`audio_play_wav` + 修 duration 公式
3. 改 `audio_player.h`：加 `audio_player_reset_dsp` 声明
4. 改 `tts_player.c`：句首调用 `audio_player_reset_dsp`
5. 改 `app_config.h`：加 DSP/AGC 调参宏
6. 改 `CMakeLists.txt`：加 `audio_dsp.c`
7. `idf.py build` → 烧录 → 听感验证 → 按需调 `AGC_TARGET_RMS`/`AGC_TAU_*`
