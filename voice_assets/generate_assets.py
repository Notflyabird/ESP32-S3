# -*- coding: utf-8 -*-
"""用 edge-tts (微软神经网络 TTS) 生成斗地主计分系统语音素材。
输出 16kHz mono 16bit wav，可直接嵌入 ESP32 flash 播放。

依赖: pip install edge-tts soundfile scipy numpy
用法:
  py generate_assets.py          # 生成/转换全部素材（含裁剪）
  py generate_assets.py --trim   # 仅对已存在 wav 原地裁剪首尾静音（无需网络）

说明: 不依赖 ffmpeg（精简版无 mp3 demuxer）。MP3 读取用 soundfile，
      重采样用 scipy，WAV 写出用 soundfile。已存在的 mp3 会复用，跳过 TTS。
      所有素材在写出前裁掉 edge-tts 的 ~400ms 首尾静音，使拼接播放更紧凑自然。
"""
import asyncio
import os
import sys

import numpy as np
import soundfile as sf
from scipy.signal import resample_poly

try:
    import edge_tts
    _HAVE_EDGE_TTS = True
except Exception:
    _HAVE_EDGE_TTS = False

VOICE = "zh-CN-XiaoxiaoNeural"   # 晓晓(女声,亲切清晰)，可改 YunjianNeural(男声沉稳)
TARGET_SR = 16000
OUT_DIR = os.path.dirname(os.path.abspath(__file__))

# 首尾静音裁剪参数
TRIM_THRESHOLD = 0.02   # |sample| 阈值（相对归一化峰值），低于此视为静音
TRIM_MARGIN_MS = 30     # 裁剪后两端保留边距（ms），避免切到音头/音尾

# 素材清单: wav文件名 -> 合成文本
ASSETS = {
    # ===== 固定话术(整句) =====
    "boot.wav":          "语音斗地主计分系统已启动",
    "im_here.wav":       "我在",
    "score_reset.wav":   "分数已重置",
    "nothing_undo.wav":  "没有可撤销的计分",
    "undo_timeout.wav":  "撤销时间已超过十秒",
    "view_log.wav":      "进入日志查看",
    "clear_log.wav":     "计分日志已清空",
    "log_exit.wav":      "返回主页",
    "unclear.wav":       "没听清请再说一次",
    # ===== 玩家 =====
    "p1.wav": "一号",
    "p2.wav": "二号",
    "p3.wav": "三号",
    # ===== 动作词 =====
    "landlord.wav": "地主",
    "win.wav":      "赢",
    "lose.wav":     "输",
    "fen.wav":      "分",
    # ===== 模板词 =====
    "cur_score.wav": "当前分数",
    "undone.wav":    "已撤销",
    "restore.wav":   "分数恢复为",
    # ===== 数字(运行时拼接: 12->十+二, 100->一+百, -5->负+五) =====
    "d0.wav": "零", "d1.wav": "一", "d2.wav": "二", "d3.wav": "三", "d4.wav": "四",
    "d5.wav": "五", "d6.wav": "六", "d7.wav": "七", "d8.wav": "八", "d9.wav": "九",
    "d10.wav": "十", "d100.wav": "百", "neg.wav": "负",
}


async def gen_one(text, mp3_path):
    """生成单个 mp3，带重试。若 mp3 已存在且 >1KB 则复用。"""
    if os.path.exists(mp3_path) and os.path.getsize(mp3_path) > 1024:
        return True
    if not _HAVE_EDGE_TTS:
        print("NO_EDGE_TTS", end="")
        return False
    for attempt in range(3):
        try:
            comm = edge_tts.Communicate(text, VOICE)
            await comm.save(mp3_path)
            if os.path.getsize(mp3_path) > 1024:
                return True
        except Exception as e:
            print(f" retry{attempt + 1}: {e}", end="", flush=True)
            await asyncio.sleep(1)
    return False


def trim_silence(data, sr):
    """裁掉首尾静音，两端保留 TRIM_MARGIN_MS 边距。data 为 float 一维数组。"""
    if data.size == 0:
        return data
    abs_data = np.abs(data)
    idx = np.where(abs_data > TRIM_THRESHOLD)[0]
    if idx.size == 0:
        return data  # 全静音，原样返回
    margin = int(sr * TRIM_MARGIN_MS / 1000)
    start = max(0, int(idx[0]) - margin)
    end = min(data.size, int(idx[-1]) + margin + 1)
    return data[start:end]


def mp3_to_wav(mp3_path, wav_path):
    """用 soundfile 读 mp3，重采样到 16kHz mono 16bit wav（含首尾裁剪）。"""
    try:
        data, sr = sf.read(mp3_path, always_2d=False, dtype="float64")
    except Exception as e:
        print(f" READ_FAIL({e})", end="")
        return False

    # 多声道 -> 单声道
    if data.ndim > 1:
        data = data.mean(axis=1)

    # 重采样到目标采样率
    if sr != TARGET_SR:
        # resample_poly 需要 up/down 互质；用 gcd 化简
        from math import gcd
        g = gcd(sr, TARGET_SR)
        up = TARGET_SR // g
        down = sr // g
        data = resample_poly(data, up, down)

    # 裁掉首尾静音（在归一化前用相对阈值）
    data = trim_silence(data, TARGET_SR)

    # 归一化避免削波，再转 int16
    peak = float(np.max(np.abs(data))) if data.size else 0.0
    if peak > 0:
        data = data / peak * 0.96
    pcm = (data * 32767.0).astype(np.int16)

    try:
        sf.write(wav_path, pcm, TARGET_SR, subtype="PCM_16")
    except Exception as e:
        print(f" WRITE_FAIL({e})", end="")
        return False

    return os.path.exists(wav_path) and os.path.getsize(wav_path) > 1000


def trim_wav_inplace(wav_path):
    """读取已存在 wav，裁剪首尾静音后原位重写。返回 (before_s, after_s)。"""
    try:
        data, sr = sf.read(wav_path, always_2d=False, dtype="float64")
    except Exception as e:
        return (0.0, 0.0, f"READ_FAIL({e})")

    if data.ndim > 1:
        data = data.mean(axis=1)
    if sr != TARGET_SR:
        from math import gcd
        g = gcd(sr, TARGET_SR)
        data = resample_poly(data, TARGET_SR // g, sr // g)
        sr = TARGET_SR

    before_s = data.size / sr
    data = trim_silence(data, sr)
    after_s = data.size / sr

    # 保留原归一化（已裁剪，重新按峰值归一以保一致）
    peak = float(np.max(np.abs(data))) if data.size else 0.0
    if peak > 0:
        data = data / peak * 0.96
    pcm = (data * 32767.0).astype(np.int16)
    sf.write(wav_path, pcm, sr, subtype="PCM_16")
    return (before_s, after_s, None)


def make_silence(wav_path, seconds=0.2):
    """生成指定时长的 16kHz mono 16bit 静音 wav（不依赖 ffmpeg）。"""
    n = int(TARGET_SR * seconds)
    pcm = np.zeros(n, dtype=np.int16)
    sf.write(wav_path, pcm, TARGET_SR, subtype="PCM_16")
    return os.path.exists(wav_path)


async def main():
    print(f"Voice : {VOICE}")
    print(f"Output: {OUT_DIR}")
    print(f"Assets: {len(ASSETS)}  edge_tts: {_HAVE_EDGE_TTS}\n")

    ok = 0
    for wav_name, text in ASSETS.items():
        mp3_path = os.path.join(OUT_DIR, wav_name.replace(".wav", ".mp3"))
        wav_path = os.path.join(OUT_DIR, wav_name)
        print(f"[{wav_name:16s}] {text}", end=" ... ", flush=True)
        if not await gen_one(text, mp3_path):
            print("FAILED(tts)")
            continue
        if mp3_to_wav(mp3_path, wav_path):
            # 转换成功后删除 mp3
            try:
                os.remove(mp3_path)
            except OSError:
                pass
            print("OK")
            ok += 1
        else:
            print("FAILED(wav)")

    # 200ms 静音(词间停顿)
    silence = os.path.join(OUT_DIR, "silence.wav")
    if make_silence(silence, 0.2):
        print(f"silence.wav ... OK")
    else:
        print(f"silence.wav ... FAILED")

    print(f"\n完成: {ok}/{len(ASSETS)} 素材 + silence.wav")


def main_trim():
    """--trim 模式：对已存在 wav 原地裁剪首尾静音（无需网络）。"""
    print(f"Trim mode: threshold={TRIM_THRESHOLD} margin={TRIM_MARGIN_MS}ms\n")
    files = sorted(f for f in os.listdir(OUT_DIR)
                   if f.endswith(".wav") and f != "silence.wav")
    total_before = total_after = 0.0
    for fn in files:
        path = os.path.join(OUT_DIR, fn)
        before, after, err = trim_wav_inplace(path)
        total_before += before
        total_after += after
        if err:
            print(f"[{fn:16s}] FAILED {err}")
        else:
            print(f"[{fn:16s}] {before:5.2f}s -> {after:5.2f}s")
    print(f"\n总计: {total_before:.2f}s -> {total_after:.2f}s "
          f"({(1 - total_after / total_before) * 100:.0f}% 裁剪)")


if __name__ == "__main__":
    if "--trim" in sys.argv:
        main_trim()
    else:
        asyncio.run(main())
