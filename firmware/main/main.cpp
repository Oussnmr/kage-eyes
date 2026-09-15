#include "bsp/esp-bsp.h"
#include "esp_err.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs_flash.h"
#include "app_ui.h"
#include "orientation_service.h"
#include "wifi_service.h"

extern "C" void app_main(void) {
    esp_err_t nvs = nvs_flash_init();
    if (nvs == ESP_ERR_NVS_NO_FREE_PAGES || nvs == ESP_ERR_NVS_NEW_VERSION_FOUND) { ESP_ERROR_CHECK(nvs_flash_erase()); ESP_ERROR_CHECK(nvs_flash_init()); }
    else ESP_ERROR_CHECK(nvs);
    lv_display_t *display = bsp_display_start();
    if (!display) { ESP_LOGE("kage", "AMOLED/LVGL startup failed"); return; }
    ESP_ERROR_CHECK(bsp_display_brightness_set(80));
    wifi_service_begin();
    if (bsp_display_lock(1000)) {
        /* Landscape by default. The IMU service later switches only between
           the two usable landscape orientations (left and right). */
        bsp_display_rotate(display, LV_DISPLAY_ROTATION_90);
        app_ui_begin();
        bsp_display_unlock();
    }
    else ESP_LOGE("kage", "LVGL lock unavailable");
    orientation_service_begin(display);
    while (true) vTaskDelay(pdMS_TO_TICKS(1000));
}
