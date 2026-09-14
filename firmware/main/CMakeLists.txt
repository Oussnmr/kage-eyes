idf_component_register(
    SRCS "main.cpp" "eyes/robot_eyes.cpp" "ui/app_ui.cpp" "services/wifi_service.cpp"
    INCLUDE_DIRS "." "eyes" "ui" "services"
    REQUIRES esp32_s3_touch_amoled_1_8 lvgl__lvgl nvs_flash esp_wifi esp_event esp_netif esp_driver_usb_serial_jtag
)

# Same strict-warning compatibility setting used by the official Waveshare demo.
idf_build_get_property(build_components BUILD_COMPONENTS)
if("lvgl__lvgl" IN_LIST build_components)
    idf_component_get_property(LVGL_LIB lvgl__lvgl COMPONENT_LIB)
    if(TARGET ${LVGL_LIB})
        target_compile_options(${LVGL_LIB} PRIVATE -Wno-error=attributes)
    endif()
endif()
