#include "ota_service.h"

#include <atomic>

#include "esp_crt_bundle.h"
#include "esp_https_ota.h"
#include "esp_log.h"
#include "esp_system.h"
#include "event_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "wifi_service.h"

namespace {
constexpr char OTA_URL[] = "https://oussnmr.github.io/kage-eyes/firmware/app.bin";
constexpr char TAG[] = "kage-ota";

std::atomic<bool> s_running{false};
const char *s_status = "READY";

void set_status(const char *status) {
    s_status = status;
    ESP_LOGI(TAG, "%s", status);
    event_log_add("OTA: %s", status);
}

void ota_task(void *) {
    set_status("DOWNLOADING");
    esp_http_client_config_t http = {};
    http.url = OTA_URL;
    http.crt_bundle_attach = esp_crt_bundle_attach;
    http.timeout_ms = 30000;
    http.keep_alive_enable = true;
    esp_https_ota_config_t ota = {};
    ota.http_config = &http;
    const esp_err_t result = esp_https_ota(&ota);
    if (result == ESP_OK) {
        set_status("RESTARTING");
        vTaskDelay(pdMS_TO_TICKS(250));
        esp_restart();
    }

    ESP_LOGE(TAG, "OTA failed: %s", esp_err_to_name(result));
    event_log_add("OTA failed: %s", esp_err_to_name(result));
    s_status = "FAILED - CHECK LOGS";
    s_running.store(false);
    vTaskDelete(nullptr);
}
}  // namespace

bool ota_service_start(void) {
    if (!wifi_service_connected()) {
        s_status = "OFFLINE";
        event_log_add("OTA refused: Wi-Fi offline");
        return false;
    }
    bool expected = false;
    if (!s_running.compare_exchange_strong(expected, true)) {
        s_status = "ALREADY RUNNING";
        return false;
    }
    set_status("STARTING");
    if (xTaskCreate(ota_task, "kage_ota", 8192, nullptr, 4, nullptr) != pdPASS) {
        s_running.store(false);
        s_status = "TASK FAILED";
        event_log_add("OTA task creation failed");
        return false;
    }
    return true;
}

const char *ota_service_status(void) {
    return s_status;
}
