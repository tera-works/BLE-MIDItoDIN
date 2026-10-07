idf_component_register(SRCS "main.c"
                       INCLUDE_DIRS "."
                       REQUIRES app_config ble_midi_client ble_midi_parser midi_uart latency_debug oled_ui serial_console
                       PRIV_REQUIRES esp_system nvs_flash bt)
