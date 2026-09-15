idf_component_register(
    SRCS "main.cpp" "app_shell.cpp" "audio/mic_meter.cpp"
         "eyes/robot_eyes.cpp" "services/orientation_service.cpp"
         "services/battery_monitor.cpp"
    INCLUDE_DIRS "." "audio" "eyes" "services"
    REQUIRES esp32_s3_touch_amoled_1_8 qmi8658 lvgl__lvgl esp_timer
             esp_driver_i2c esp_codec_dev fatfs sdmmc
)

# Same strict-warning compatibility setting used by the official Waveshare demo.
idf_build_get_property(build_components BUILD_COMPONENTS)
if("lvgl__lvgl" IN_LIST build_components)
    idf_component_get_property(LVGL_LIB lvgl__lvgl COMPONENT_LIB)
    if(TARGET ${LVGL_LIB})
        target_compile_options(${LVGL_LIB} PRIVATE -Wno-error=attributes)
    endif()
endif()
