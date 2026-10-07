idf_component_register(SRCS "ble_midi_client.c"
                       INCLUDE_DIRS "include"
                       REQUIRES ble_midi_parser app_config
                       PRIV_REQUIRES bt latency_debug midi_uart nvs_flash esp_hw_support)
