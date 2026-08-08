# 预生成语音素材接入固件（替换 esp-tts）

## Context（背景）

当前系统用 esp-tts（xiaole 音色）离线合成中文语音，存在两个核心问题：
1. **音质差**：合成声与真人区别大，机械感强。
2. **稳定性差**：`esp_tts_voice_set_init` 曾触发 `LoadProhibited` panic 导致复位；`esp_tts_parse_chinese` 需 >16KB 栈。

现已用 edge-tts（微软神经网络 zh-CN-XiaoxiaoNeural）预生成 28 条 16kHz/mono/16bit WAV 素材，存放于 `voice_assets/`。本次目标：把素材嵌入 flash，新建素材播放模块替换 esp-tts，所有播报改用预生成的高质量音频拼接播放。同时移除 esp-tts 引擎及其 2.94MB 的 `xiaole.dat`，彻底消除崩溃源并大幅节省 flash。

预期结果：开机播报、唤醒应答、计分/查询/撤销语音全部为自然神经声音；不再调用任何 `esp_tts_*` API。

## 方案概要

- **新文件**：`src/voice_assets.{h,c}`（嵌入 WAV 表 + 渲染）、`src/voice_player.{h,c}`（任务/队列/编排 + 公共 speak API）。
- **删除**：`src/tts_player.{h,c}`。
- **迁移**：`main.c`、`speech_recognition.c`、`scorekeeper.c` 把 `tts_play_text` → 结构化 `voice_speak_*`。
- **构建**：`src/CMakeLists.txt` 换源文件、删 `xiaole.dat` 嵌入、加 28 条 WAV `target_add_binary_data`。
- **素材修剪**：更新 `generate_assets.py` 加 `--trim` 模式，裁掉 edge-tts 的 ~400ms 首尾静音（否则单字素材 ~1s，拼接会拖沓断续）。

## 关键复用点

- [audio_player.c:291](file:///d:/03_project/esp32/01_code/esp32_s3_0725/ESP32-S3/src/audio_player.c) `audio_play_pcm(const int16_t*, size_t sample_count)` —— sample_count 为**采样点数**；走 DC→AGC→3×上采样→tanh→stereo→阻塞 I2S 写。**不重置 DSP**，适合整句一次性播放。
- [audio_player.c:205](file:///d:/03_project/esp32/01_code/esp32_s3_0725/ESP32-S3/src/audio_player.c) `audio_play_wav` 的 WAV 头解析（RIFF/WAVE/fmt/data chunk 搜索）—— 拷贝到 voice_assets.c 复用，校验 mono+16bit。
- [audio_player.c:139](file:///d:/03_project/esp32/01_code/esp32_s3_0725/ESP32-S3/src/audio_player.c) `audio_set_mute`、[audio_player.c:155](file:///d:/03_project/esp32/01_code/esp32_s3_0725/ESP32-S3/src/audio_player.c) `audio_player_reset_dsp` —— 编排复用。
- DMA 缓冲 = 1024帧×6描述符×4B = 24KB = **128ms** @48kHz stereo。阻塞写按实时节拍，播放返回后 ≤128ms 残留 → 句末 `vTaskDelay(200ms)` 排空即可（**不要**用 `audio_wait_play_finish`，长句会按全句时长过等）。
- `g_sr_paused` 原定义在 [tts_player.c:172](file:///d:/03_project/esp32/01_code/esp32_s3_0725/ESP32-S3/src/tts_player.c)，`speech_recognition.c:21` extern 使用 —— 定义迁到 `voice_player.c`，extern 声明不变。

## 实施步骤

### Step 0：修剪 WAV 素材（离线，不改固件）
更新 `voice_assets/generate_assets.py`：
- 新增 `trim_wav(path)`：`soundfile.read(dtype="float64")` → 找 `|sample|>0.02` 的首/末 → 两端留 30ms(480样本) 边距 → 原位重写（保留原 0.96 峰值归一化，不再重归一化）。
- `mp3_to_wav` 路径也集成裁剪（未来重生即已裁剪）。
- 加 `--trim` 参数：遍历 OUT_DIR 所有 `*.wav`（排除 `silence.wav`）原地裁剪，打印前后时长。
- 执行 `py generate_assets.py --trim`。预期单字素材 ~1s → ~0.3-0.5s，总量 1.24MB → ~450KB。

### Step 1：新建 `src/voice_assets.h`
- 枚举 `voice_asset_id_t`（28 值 + `VOICE_ASSET_COUNT`）：BOOT/IM_HERE/SCORE_RESET/NOTHING_UNDO/UNDO_TIMEOUT、P1/P2/P3、LANDLORD/WIN/LOSE/FEN、CUR_SCORE/UNDONE/RESTORE、D0..D9/D10/D100/NEG。
- `#define VOICE_GAP_MS 60`（词间静音）、`VOICE_GAP_SAMPLES`（=16000*60/1000=960）、`VOICE_SEQ_MAX 48`。
- API：`const int16_t* voice_asset_pcm(id, size_t*sample_count)`、`void voice_play_sequence(const voice_asset_id_t*ids, size_t count)`、`size_t voice_assets_append_number(ids, count, cap, int value)`。

### Step 2：新建 `src/voice_assets.c`
- 28 对 `extern const uint8_t _binary_<name>_wav_start[]/_end[]`（符号名 = 文件名点→下划线，如 `d10.wav`→`_binary_d10_wav_start`）。
- 静态表 `s_assets[VOICE_ASSET_COUNT]` = {start, end, name}。
- `voice_asset_pcm`：复用 audio_play_wav 的解析逻辑，返回 data chunk 内的 `const int16_t*` 与采样数。
- `voice_assets_append_number`：中文读数 -999..999，规则：
  - 负数 → NEG + 绝对值；0 → D0；>999 → 逐位读（fallback）。
  - 百位 h>0：D(h)+D100；余 r<10 且>0 → 插 D0（一百零X）；r≥10 → D(十位)+D10（百位存在时十位为1也读"一十"）+ D(个位)。
  - 无百位：10-19 → D10(+D个位，不读一十)；20-99 → D(十位)+D10(+D个位)。
- `voice_play_sequence`：
  1. 总采样 = Σ各素材 + (count-1)×VOICE_GAP_SAMPLES。
  2. `heap_caps_malloc(total*2, MALLOC_CAP_SPIRAM)`；失败回退：逐素材 `audio_play_pcm` + 静态零缓冲 `audio_play_pcm(s_zero_gap, 960)`。
  3. 成功：memset 清零 → 逐素材 memcpy，词间留 960 零样本 → `audio_play_pcm(buf, total)` 一次 → free。

### Step 3：新建 `src/voice_player.h` / `src/voice_player.c`
- 定义 `volatile bool g_sr_paused = false;`（从 tts_player.c 迁来）。
- 任务栈 8192、优先级 5、core 1、队列长 4（无 esp-tts 不再需 32KB 栈）。
- `voice_job_t`：type 枚举 + union（score_update/query/undo_result 字段）。
- 公共 API（全部非阻塞入队，`s_playing=true` 后 `xQueueSend`）：
  `voice_speak_boot/im_here/reset/undo_none/undo_timeout/score_update(player,win,points)/query(s1,s2,s3)/undo_result(was_reset,op_player,op_win,op_points,s1,s2,s3)`、`voice_player_init`、`voice_is_playing`、`voice_player_deinit`。
- `voice_task` 循环：`g_sr_paused=true`→`vTaskDelay(50ms)`→`audio_set_mute(false)`→`audio_player_reset_dsp()`→按 job.type 组 `ids[VOICE_SEQ_MAX]`→`voice_play_sequence`→`vTaskDelay(200ms)`→`audio_set_mute(true)`→`g_sr_paused=false`→`s_playing=false`。
- 序列组装（在 voice_task 内）：
  - SCORE_UPDATE：`{P(player), LANDLORD, win?WIN:LOSE}` + append_number(points) + `{FEN}`
  - QUERY：`{CUR_SCORE}` + 3×`{P(i), append_number(s_i), FEN}`
  - UNDO_RESULT：was_reset → `{UNDONE, RESTORE}` + 3×`{P,append_number,FEN}`；否则 → `{UNDONE, P(op), LANDLORD, op_win?WIN:LOSE}` + append_number(op_points) + `{FEN, CUR_SCORE}` + 3×`{P,append_number,FEN}`
  - `P(i)` = `VOICE_ASSET_P1 + (i-1)`
  - 最坏序列（undo_result 非重置 + 三个 -999）≈ 41 id < 48，安全。

### Step 4：删除 `src/tts_player.{h,c}`

### Step 5：迁移 `src/main.c`
`#include "tts_player.h"`→`voice_player.h`；`tts_player_init()`→`voice_player_init()`；`tts_play_text("语音斗地主计分系统已启动")`→`voice_speak_boot()`；`tts_is_playing()`→`voice_is_playing()`。

### Step 6：迁移 `src/speech_recognition.c`
`#include "tts_player.h"`→`voice_player.h`；`extern volatile bool g_sr_paused;` 不变；`tts_play_text("我在")`→`voice_speak_im_here()`。
（`undo_button.c` 不含 tts 头、只调 `scorekeeper_undo_last()`，无需改动。）

### Step 7：迁移 `src/scorekeeper.c`（改动最大）
- include 换 `voice_player.h`。
- `undo_snapshot_t`：`char desc[64]` → 结构化字段 `int op_player; bool op_landlord_win; int op_points;`。
- `undo_snapshot_commit` 签名改为接收结构化 op（去掉 va_list/vsnprintf）；调用点同步更新（score case 传 `player/landlord_win/points`，reset case 传 `0/false/0`）。
- `speak_score_update/speak_query_score/speak_reset` 体改为调对应 `voice_speak_*`。
- `scorekeeper_undo_last`：none→`voice_speak_undo_none()`；timeout→`voice_speak_undo_timeout()`；成功→释放锁前拷结构化字段，调 `voice_speak_undo_result(was_reset, op_player, op_landlord_win, op_points, s1,s2,s3)`；删 `desc_saved`/`speak[160]`/snprintf；`ESP_LOGI` 改打结构化字段。
- 清理仅服务于 va_list 的 `<stdarg.h>`（若他处未用）。

### Step 8：更新 `src/CMakeLists.txt`
- SRCS：删 `tts_player.c`，加 `voice_player.c`、`voice_assets.c`。
- 删 `target_add_binary_data(... esp_tts_voice_data_xiaole.dat ...)`（省 2.94MB）。
- 加 28 条 `target_add_binary_data(${COMPONENT_LIB} "${CMAKE_CURRENT_SOURCE_DIR}/../voice_assets/<name>.wav" BINARY)`（boot/im_here/score_reset/nothing_undo/undo_timeout/p1/p2/p3/landlord/win/lose/fen/cur_score/undone/restore/d0..d9/d10/d100/neg；**不含 silence.wav**）。
- `PRIV_REQUIRES` 保留 `esp-sr`（SR 仍需 WakeNet/MultiNet/AFE）。

## 风险与缓解
| 风险 | 缓解 |
|------|------|
| PSRAM 分配失败 | 回退逐素材 `audio_play_pcm` + 静态零缓冲，维持 DSP 连续 |
| 裁剪过头切到音头/音尾 | 0.02 阈值 + 30ms 边距偏保守，听感验证 |
| WAV 符号名不匹配链接报错 | 已核验：点→下划线、数字保留（d10.wav→`_binary_d10_wav_start`） |
| 60ms 词间隙听感断续 | 先 60ms，若数字读法断续降到 30ms（`VOICE_GAP_MS` 可调） |
| AGC 在零间隙漂移 | attack 20ms 快速修正；素材已预归一化，漂移小 |

## 验证
1. **构建**：`idf.py build` 无错；app 体积 ≈ 830KB − 2.94MB + ~450KB，app0(4MB) 充裕；28 个 `_binary_*_wav` 符号全部解析。
2. **开机**：烧录启动，播报"语音斗地主计分系统已启动"为自然神经声；**无** `esp_tts_voice_set_init` 崩溃；`voice_is_playing()` 正确阻塞 main 直到播完再起 SR。
3. **唤醒**："Hi ESP" → "我在"（im_here.wav）。
4. **计分**："一号地主赢六分" → p1+landlord+win+d6+fen 拼接自然；测 2/10/20 分点。
5. **查询**："查询分数" → "当前分数一号X分…"；测 0、正、负分。
6. **重置**："重置所有分数" → "分数已重置"。
7. **撤销**：10s 内按键 → "已撤销一号地主赢六分当前分数…"；撤销重置 → "已撤销分数恢复为…"；无操作 → "没有可撤销的计分"；超 10s → "撤销时间已超过十秒"。
8. **读数边界**：0→零、10→十(非一十)、20→二十、100→一百、101→一百零一、110→一百一十、-3→负三。
9. **SR 交互**：播放期间 feed_task 暂停（无误唤醒）；播完 200ms 排空+静音后恢复唤醒。
10. **音质**：句首/句末无爆音（DSP 重置+渐变静音时序保留）；句中无断续（单次 audio_play_pcm，词间不重置 DSP）。
