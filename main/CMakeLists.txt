idf_component_register(
    SRCS "main.c"
    INCLUDE_DIRS "."
    REQUIRES
        driver
        esp_driver_i2s
        esp_timer
        esp_lcd
        esp_system
        spi_flash
)
 
target_link_libraries(${COMPONENT_LIB} INTERFACE m)
