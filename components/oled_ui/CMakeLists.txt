idf_component_register(SRCS "oled_ui.c" "oled_frame.c" "oled_transfer.c" "encoder_input.c"
    INCLUDE_DIRS "include"
    REQUIRES app_config ble_midi_client midi_uart latency_debug
    PRIV_REQUIRES esp_driver_i2c esp_driver_gpio freertos)
