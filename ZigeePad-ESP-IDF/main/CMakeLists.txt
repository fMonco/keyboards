idf_component_register(SRCS "main.c" "buttons.c" "chords.c" "rtc_wake_stub_micropad.c"
                       INCLUDE_DIRS "."
                       REQUIRES esp_driver_gpio esp_timer nvs_flash esp-zigbee-lib)
