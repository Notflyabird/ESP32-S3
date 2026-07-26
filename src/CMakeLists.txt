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
    INCLUDE_DIRS "."
    PRIV_REQUIRES driver esp_driver_i2s esp_driver_spi
)
