#include "bsp/esp-bsp.h"
#include "esp_err.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "orientation_service.h"
#include "robot_eyes.h"

extern "C" void app_main(void) {
    lv_display_t *display = bsp_display_start();
    if (!display) { ESP_LOGE("kage", "AMOLED/LVGL startup failed"); return; }
    ESP_ERROR_CHECK(bsp_display_brightness_set(80));
    if (bsp_display_lock(1000)) {
        bsp_display_rotate(display, LV_DISPLAY_ROTATION_90);
        robot_eyes_begin(lv_screen_active());
        bsp_display_unlock();
    }
    else ESP_LOGE("kage", "LVGL lock unavailable");
    orientation_service_begin(display);
    while (true) vTaskDelay(pdMS_TO_TICKS(1000));
}
