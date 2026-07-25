idf_component_register(
    SRCS
        "main.c"
        "audio_input.c"
        "scorekeeper.c"
        "speech_recognition.c"
        "lcd_st7789.c"
    INCLUDE_DIRS "."
    PRIV_REQUIRES driver esp_driver_i2s esp_driver_spi
)
