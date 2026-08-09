# 计分日志持久化 + 屏幕查看 + 语音清除 实施计划

## Context（背景）

当前斗地主计分系统的三人分数 (`s_score[3]`/`s_landlord`) 与每局记录**仅存在 RAM**，掉电即丢；项目虽已有 20KB `nvs` 分区但完全未使用。用户需要：
1. 每一局计分记录掉电保存，开机恢复（最近 256 局环形缓冲）；
2. 当前三人分数也一并持久化、开机恢复；
3. 语音"查看计分日志"进入日志页，GPIO0 短按翻页、长按或 30s 超时回主页（主页时 GPIO0 仍为撤销）；
4. 语音"清除计分日志"= 清空全部历史日志 + 当前分数归零（全新开始）。

预期结果：掉电不丢分、不丢历史；可用语音随时查看与清除日志。

## 架构：新增 `score_log` 模块

新增 [src/score_log.c](file:///d:/03_project/esp32/01_code/esp32_s3_0725/ESP32-S3/src/score_log.c) / [src/score_log.h](file:///d:/03_project/esp32/01_code/esp32_s3_0725/ESP32-S3/src/score_log.h)，集中管理：NVS 读写、RAM 环形缓冲、日志页视图状态机。scorekeeper 在计分/撤销/清除时调用其 API；undo_button 在日志页态调用其视图 API。

### 数据结构（packed，与 NVS blob 二进制布局严格一致）

```c
typedef struct __attribute__((packed)) {
    uint16_t round_no;          // 局号 1..65535(回绕至1)
    uint8_t  op;                // 0=计分 1=重置
    uint8_t  player;            // 1..3 (重置时0)
    uint8_t  landlord;          // 1..3 (重置时0)
    uint8_t  landlord_win;      // 0/1
    uint8_t  points;            // 2..20 (重置时0)
    int16_t  scores_after[3];   // 本局结算后总分快照
} score_log_entry_t;            // 14 字节

typedef struct __attribute__((packed)) {
    uint16_t round_no_next;     // 下一条局号
    uint16_t write_idx;         // 写入位置 0..255
    uint16_t count;             // 有效条数 0..256
    uint16_t reserved;
    score_log_entry_t entries[256];
} score_log_blob_t;             // 8 + 256×14 = 3592 字节

typedef struct __attribute__((packed)) {
    int32_t scores[3];
    int32_t landlord;
} score_cur_blob_t;             // 16 字节
```

### NVS 设计

- Namespace `"ddz_score"`；Key `"log"` (blob 3592B)、Key `"scores"` (blob 16B)。20KB nvs 分区容量充足。
- 视图状态（active/page_index/num_pages/last_activity_ms）仅 RAM，不持久化。
- `nvs_flash_init()` 放 `app_main` 最开头，按 ESP-IDF 标准模式处理 `ESP_ERR_NVS_NO_FREE_PAGES`/`NEW_VERSION_FOUND`（erase 后重建）。
- 任何 NVS 失败 → `s_persist_disabled=true` 降级（仅 RAM，不阻塞启动，不 panic）。

### score_log.h 公共 API

```c
bool score_log_init(void);                         // 开NVS、读blob、调 scorekeeper_restore_state 恢复分数
void score_log_append_round(uint16_t rnd, uint8_t player, uint8_t landlord,
                            bool win, uint8_t points, int s1,int s2,int s3);  // 调用方持 s_score_lock
void score_log_append_reset(uint16_t rnd, int s1,int s2,int s3);              // 调用方持 s_score_lock
void score_log_persist_scores_locked(int s1,int s2,int s3,int landlord);      // 撤销后同步当前分数
void score_log_clear_all(void);                    // 清RAM+擦NVS log key+写scores=0 (CMD_CLEAR_LOG)
uint16_t score_log_next_round_no(void);            // 原子递增返回下个局号
bool score_log_view_is_active(void);
void score_log_view_enter(void);                   // 进入日志页(重绘第1页)
void score_log_view_next_page(void);               // 短按:下一页回绕
void score_log_view_exit(void);                    // 长按/超时退出(重绘主页)
```

## 各文件改动

### 1. src/scorekeeper.c / .h（计分集成）
- 新增命令 id：`CMD_VIEW_LOG=3`、`CMD_CLEAR_LOG=4`。
- `scorekeeper_register_commands()` 末尾追加注册：`"cha kan ji fen ri zhi"`、`"qing chu ji fen ri zhi"`。
- `undo_snapshot_commit()` 内（持 `s_score_lock`）追加：`score_log_next_round_no()` + `score_log_append_round/reset()` + `score_log_persist_scores_locked()`。
- `scorekeeper_undo_last()` 回滚分数后追加 `score_log_persist_scores_locked()`（撤销不进日志条目，仅同步当前分数）。
- `apply_command` 新增分支：
  - `CMD_VIEW_LOG` → 若已在日志页则重置页码重绘，否则 `score_log_view_enter()` + `voice_speak_view_log()`。
  - `CMD_CLEAR_LOG` → 取锁、`reset_scores()`、`score_log_clear_all()`、`s_last_op.valid=false`、释锁；若在日志页先 `view_exit()`；`lcd_ui_update(0,0,0,0,"All cleared")` + `voice_speak_clear_log()`。
  - **所有计分/查询/重置分支开头**：若 `score_log_view_is_active()` 先 `score_log_view_exit()`（日志页期间收到命令自动回主页再处理）。
- 新增 `scorekeeper_restore_state(int scores[3], int landlord)`：设 `s_score`/`s_landlord`，**强制 `s_last_op.valid=false`**（跨重启不可撤销），由 `score_log_init` 开机调用。
- 复用现有 `s_score_lock` 保护所有计分路径的 score_log 写访问（写日志路径不再单独取 log_lock，避免嵌套）。

### 2. src/undo_button.c（GPIO0 双模式 + 长短按 + 超时）
- ISR 不变（已发双边沿事件，`pressed=(level==0)`）。
- 重写 `undo_button_task`：
  - **主页态**（`!score_log_view_is_active()`）：收到 `pressed` → 立即 `scorekeeper_undo_last()`（保持原快速响应，不等释放）。释放忽略。
  - **日志页态**：收到 `pressed` 记 `press_start_ms`，进入长短按判定；收到 `release` 计算时长：`<800ms`→`score_log_view_next_page()`，`≥800ms`→`score_log_view_exit()`。
  - **30s 超时**：日志页态用 `xQueueReceive(..., timeout)`，timeout=`30000-(now-last_activity_ms)`；超时→`score_log_view_exit()`（静默）。长按判定也用同一路超时机制（取较小剩余时间）。
- 二次消抖（30ms）保留。

### 3. src/lcd_ui.c / .h（LCD 互斥 + 日志页）
- **新增 `s_lcd_lock` mutex**（FreeRTOS mutex，自带优先级继承）。`lcd_ui_init_page`/`lcd_ui_update`/新日志页函数入口 `lcd_lock()`、出口 `lcd_unlock()`。原因：`undo_btn` 任务（未绑核，优先级6）与 `sr_detect`（core1，优先级5）都会画 LCD，而 `spi_device_polling_transmit` 非线程安全。
- 新增 `lcd_ui_log_draw_page(uint16_t page_idx, uint8_t num_pages, const score_log_entry_t *entries, uint8_t count)`。
- 布局（240×320，8×16 字体，30字符/行）：
  - Y0 标题 `"Score Log P01/P16"`（YELLOW，居中）
  - Y16 横线；Y20 列头 `"Rnd P L Pt  P1  P2  P3"`（GREY）；Y36 横线
  - Y40 起 16 行数据，每行 `snprintf("%3d %d %c %2d %4d %4d %4d", rnd, player, win?'W':'L', pts, s1,s2,s3)`；地主行 RED，负分 RED，已撤销 GREY；reset 行显示 `"RESET"`
  - Y298 提示 `"S=Next L=Exit"`（GREY）
- 翻页：page0=最近16条（最新在顶），`num_pages=ceil(count/16)`；下一页回绕至 page0。
- `score_log_view_*` 内部：取 `s_log_lock`→memcpy 本页 16 条到栈数组→释锁→调 `lcd_ui_log_draw_page`（其内取 lcd_lock）。避免长持 log_lock 阻塞计分写入。

### 4. 语音素材（3 个新 wav）
- `view_log.wav`"进入日志查看"、`clear_log.wav`"计分日志已清空"、`log_exit.wav`"返回主页"。
- [voice_assets/generate_assets.py](file:///d:/03_project/esp32/01_code/esp32_s3_0725/ESP32-S3/voice_assets/generate_assets.py) ASSETS dict 追加 3 项；运行 `py voice_assets/generate_assets.py` 生成（已有 wav 跳过）。
- [src/CMakeLists.txt](file:///d:/03_project/esp32/01_code/esp32_s3_0725/ESP32-S3/src/CMakeLists.txt) foreach 追加 `view_log clear_log log_exit`；SRCS 追加 `score_log.c`；PRIV_REQUIRES 追加 `nvs_flash`。
- [src/voice_assets.h](file:///d:/03_project/esp32/01_code/esp32_s3_0725/ESP32-S3/src/voice_assets.h) 枚举在 `VOICE_ASSET_RESTORE` 后插入 `VOICE_ASSET_VIEW_LOG/CLEAR_LOG/LOG_EXIT`（COUNT 自动+3）。
- [src/voice_assets.c](file:///d:/03_project/esp32/01_code/esp32_s3_0725/ESP32-S3/src/voice_assets.c) extern 3 个 `_binary_*_wav_start/end` + s_assets 表追加 3 项。
- [src/voice_player.c](file:///d:/03_project/esp32/01_code/esp32_s3_0725/ESP32-S3/src/voice_player.c) / .h：新增 `VOICE_JOB_VIEW_LOG/CLEAR_LOG/LOG_EXIT` + switch case + `voice_speak_view_log()/clear_log()/log_exit()`。

### 5. src/main.c（NVS 初始化 + 恢复分数上屏）
- `app_main` 最开头：`nvs_flash_init()`（标准错误处理）。
- 紧接 `score_log_init()`（在 LCD init 之前，恢复 `s_score`/`s_landlord`）。
- LCD init 后用恢复的分数 `lcd_ui_update(landlord, s1, s2, s3, "Restored")` 替代原来的全零初始化显示。
- 加 `#include "nvs_flash.h"`、`"score_log.h"`。

## 并发与锁顺序

全局获取顺序：`s_score_lock` > `s_log_lock` > `s_lcd_lock`（防死锁）。
- 写日志路径：持 `s_score_lock`（不取 log_lock）→ 写 NVS → 经 `lcd_ui_update` 取 `s_lcd_lock`。
- 视图路径：取 `s_log_lock` → 拷贝 → 释 `s_log_lock` → 取 `s_lcd_lock` 画屏。
- `s_score_lock` 与 `s_log_lock` 不同时持有（避免长 NVS 写阻塞翻页）。
- LCD 互斥保证 `undo_btn`（任一核, prio6）与 `sr_detect`（core1, prio5）并发画屏安全。

## 文件清单

**新增**：`src/score_log.c`、`src/score_log.h`
**修改**：`src/main.c`、`src/scorekeeper.c/.h`、`src/undo_button.c`、`src/lcd_ui.c/.h`、`src/voice_assets.c/.h`、`src/voice_player.c/.h`、`src/CMakeLists.txt`、`voice_assets/generate_assets.py`
**不改**：`partitions.csv`（nvs 分区已存在）、`sdkconfig`（NVS 默认启用，无加密）

## 验证

1. **编译烧录**：`idf.py fullclean`（CMakeLists 改了 SRCS+wav）→ `idf.py build` → `idf.py -p PORT flash monitor`。
2. **首次启动**：日志见 `score_log_init` 恢复 P1=0,P2=0,P3=0；LCD 主页显示 "Restored"；播报开机语。
3. **持久化**：唤醒"你好小鑫"→"一号地主赢两分"（P1=2,P2=-1,P3=-1）→ 断电重启 → LCD 显示恢复的分数 → 按 GPIO0 应播"没有可撤销的计分"（开机后 s_last_op 失效）。
4. **翻页**：连续计分 16+ 局 →"查看计分日志"→ 进日志页 → 短按翻页（回绕）→ 长按退出 → 30s 不操作自动退出。
5. **清除**："清除计分日志"→ 播"计分日志已清空"→ 主页全零 → 重启仍为零、日志空。
6. **并发**：日志页期间说"二号地主输十分"→ 自动退日志页并处理计分；计分期间短按 GPIO0 正常撤销。
7. **NVS 损坏**：擦 nvs 分区后启动 → 自动 erase+init，降级为初始状态。

## 风险与边界

- **NVS 写延迟**：每局写 3592B blob 约 20-50ms，发生在 `undo_snapshot_commit`（持 `s_score_lock`），计分频率低，可接受。
- **flash 寿命**：每局一次写，斗地主频率远低于 flash 擦写周期限制。
- **跨重启不可撤销**：开机 `s_last_op.valid=false`，避免重启后误按 GPIO0 撤销恢复的分数（符合直觉）。
- **局号回绕**：round_no `uint16_t` 65535 后回绕至 1；write_idx/count 永不溢出（模 256）。
- **日志页与语音**：进入日志页播报期间 `g_sr_paused=true` 暂停麦克风（与现有计分播报一致）；长按退出播 `log_exit`，30s 超时静默退出。
- **LCD 线程安全**：经 `s_lcd_lock` 解决（spi_device_polling_transmit 非线程安全）。
- **内存**：`s_log` 3592B 落 BSS（内部 SRAM，512KB 充裕）；如紧张可后续迁 PSRAM。
