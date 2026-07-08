idf_component_register(
    SRCS "main.c" "ble.c" "eyesync.c"
    INCLUDE_DIRS "."
    REQUIRES
        driver
        esp_driver_i2c
        esp_driver_i2s
        esp_driver_ledc
        esp_timer
        esp_lcd
        esp_system
        spi_flash
        bt
        nvs_flash
        esp_adc
        esp_partition
        esp_app_format
)

target_link_libraries(${COMPONENT_LIB} INTERFACE m)
