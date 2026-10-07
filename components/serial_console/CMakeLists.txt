idf_component_register(SRCS "serial_console.c" INCLUDE_DIRS "include"
 REQUIRES ble_midi_client app_config PRIV_REQUIRES esp_driver_uart freertos)
