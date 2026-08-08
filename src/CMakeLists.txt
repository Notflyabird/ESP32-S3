idf_component_register(
    SRCS
        "main.c"
        "audio_input.c"
        "audio_dsp.c"
        "audio_player.c"
        "lcd_font.c"
        "lcd_st7789.c"
        "lcd_ui.c"
        "scorekeeper.c"
        "speech_recognition.c"
        "voice_player.c"
        "voice_assets.c"
        "undo_button.c"
    INCLUDE_DIRS "."
    PRIV_REQUIRES driver esp_driver_i2s esp_driver_spi esp_timer esp-sr
)

# ===== 嵌入预生成语音素材（edge-tts 神经网络 TTS，16kHz mono 16bit WAV）=====
# 替代 esp-tts xiaole 合成声，符号名 = 文件名（点→下划线）：boot.wav -> _binary_boot_wav_start
set(VOICE_ASSET_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../voice_assets")
foreach(wav
        boot im_here score_reset nothing_undo undo_timeout
        p1 p2 p3
        landlord win lose fen
        cur_score undone restore
        d0 d1 d2 d3 d4 d5 d6 d7 d8 d9 d10 d100 neg)
    target_add_binary_data(${COMPONENT_LIB} "${VOICE_ASSET_DIR}/${wav}.wav" BINARY)
endforeach()
