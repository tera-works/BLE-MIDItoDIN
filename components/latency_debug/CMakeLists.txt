idf_component_register(SRCS "latency_debug.c"
                       INCLUDE_DIRS "include"
                       REQUIRES app_config
                       PRIV_REQUIRES esp_driver_gpio esp_hal_gpio soc esp_hw_support freertos)
