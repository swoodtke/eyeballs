idf_component_register(
    SRCS "main.c"
    INCLUDE_DIRS "."
    REQUIRES
        driver
        esp_timer
        esp_lcd
        i2c
)

target_link_libraries(${COMPONENT_LIB} INTERFACE m)
