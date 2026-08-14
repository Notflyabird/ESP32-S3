#pragma once

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 低功耗画像：PM 锁控制 + 可选常亮状态。
 *
 * L3 DFS（动态降频）策略：
 *   默认 160 MHz；空闲 → DFS 自动降到 40 MHz 省电；
 *   高算力阶段（语音识别活跃期 / TTS 播报期）调 pm_profile_high_perf_acquire()
 *   临时拉到 240 MHz，完成后 release 回归节能态。
 *
 *   内部采用"计数式" acquire/release：同任务或跨任务多次 acquire 只需要
 *   对应次数 release 后才真正释放锁，避免嵌套场景下频率抖动。
 */

/** @brief 初始化 PM 锁（在 app_main 早期调用即可，重复调用安全）。*/
void pm_profile_init(void);

/**
 * @brief 进入高算力模式：acquire ESP_PM_CPU_FREQ_MAX 锁保持 240 MHz。
 *        可嵌套调用，必须调用次数对等的 _release() 才真正释放。
 */
void pm_profile_high_perf_acquire(void);

/** @brief 高算力阶段结束释放 240 MHz 锁。与 _acquire() 配对调用。*/
void pm_profile_high_perf_release(void);

/** @brief 当前是否处于"持有 240 MHz 高算力锁"的状态（调试用）。*/
bool pm_profile_is_high_perf(void);

#ifdef __cplusplus
}
#endif
