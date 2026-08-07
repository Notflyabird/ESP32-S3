idf_component_register(
    SRCS
        "main.c"
        "audio_input.c"
        "audio_player.c"
        "lcd_font.c"
        "lcd_st7789.c"
        "lcd_ui.c"
        "scorekeeper.c"
        "speech_recognition.c"
        "tts_player.c"
        "undo_button.c"
    INCLUDE_DIRS "."
    PRIV_REQUIRES driver esp_driver_i2s esp_driver_spi esp_timer esp-sr
)

target_add_binary_data(${COMPONENT_LIB} "${CMAKE_CURRENT_SOURCE_DIR}/../managed_components/espressif__esp-sr/esp-tts/esp_tts_chinese/esp_tts_voice_data_xiaole.dat" BINARY)
