idf_component_register(SRCS "midi_uart.c"
                       INCLUDE_DIRS "include"
                       REQUIRES app_config latency_debug
                       PRIV_REQUIRES esp_driver_uart esp_driver_gpio esp_hal_uart soc esp_hw_support freertos)
