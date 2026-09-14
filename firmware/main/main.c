/* Kage Eyes v1: board setup, touch and a non-blocking 60 fps render loop. */
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "nvs_flash.h"
#include "display_port.h"
#include "touch_port.h"
#include "eyes.h"
#include "battery_port.h"
#include "driver/i2c_master.h"

#define FRAME_W 448
#define FRAME_H 368
static const char *TAG = "kage-eyes";
static void on_tap(void *context) { (void)context; eyes_next_expression(); }

void app_main(void) {
    ESP_LOGI(TAG, "Kage Eyes v1 boot");
    ESP_ERROR_CHECK(nvs_flash_init());
    ESP_ERROR_CHECK(display_port_init() ? ESP_OK : ESP_FAIL);
    extern i2c_master_bus_handle_t board_i2c_bus(void);
    battery_port_init(board_i2c_bus());
    battery_port_trim_rails();
    touch_port_init();                 /* a missing touch controller must not stop the face */
    touch_port_set_tap_callback(on_tap, NULL);
    uint16_t *frame = heap_caps_calloc(FRAME_W * FRAME_H, sizeof(uint16_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!frame) frame = heap_caps_calloc(FRAME_W * FRAME_H, sizeof(uint16_t), MALLOC_CAP_8BIT);
    ESP_ERROR_CHECK(frame ? ESP_OK : ESP_ERR_NO_MEM);
    eyes_init();
    for (;;) {
        touch_port_poll();
        eyes_update(esp_timer_get_time());
        eyes_render(frame, FRAME_W, FRAME_H);
        display_port_flush(frame);
        vTaskDelay(pdMS_TO_TICKS(16)); /* yields to DMA/touch; no animation delay */
    }
}
