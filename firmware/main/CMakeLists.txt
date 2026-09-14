set(SRCS main.c eyes/eyes.c battery_port_axp2101.c)
if(CONFIG_KAGE_EYES_DISPLAY_SH8601)
    list(APPEND SRCS display_port_sh8601.c)
else()
    list(APPEND SRCS display_port_stub.c)
endif()
if(CONFIG_KAGE_EYES_TOUCH)
    list(APPEND SRCS touch_port_ft3168.c)
else()
    list(APPEND SRCS touch_port_stub.c)
endif()
idf_component_register(
    SRCS ${SRCS}
    INCLUDE_DIRS "." "eyes"
    REQUIRES esp_timer esp_driver_spi esp_driver_i2c esp_driver_gpio esp_lcd nvs_flash
)
