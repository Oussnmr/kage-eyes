#include "battery_monitor.h"

#include <atomic>
#include <cstdint>

#include "esp_err.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

namespace {
constexpr char TAG[] = "kage-battery";
constexpr uint8_t AXP2101_ADDRESS = 0x34;
constexpr uint8_t REG_STATUS1 = 0x00;
constexpr uint8_t REG_STATUS2 = 0x01;
constexpr uint8_t REG_PERCENT = 0xA4;

static i2c_master_dev_handle_t s_device;
static std::atomic<float> s_level{0.0f};
static std::atomic<bool> s_charging{false};
static std::atomic<bool> s_valid{false};

static bool read_register(uint8_t reg, uint8_t *value) {
    return s_device && i2c_master_transmit_receive(s_device, &reg, 1, value, 1, 100) == ESP_OK;
}

static void monitor_task(void *) {
    while (true) {
        uint8_t status1 = 0;
        uint8_t status2 = 0;
        uint8_t percent = 0;
        const bool ok = read_register(REG_STATUS1, &status1) &&
                        read_register(REG_STATUS2, &status2) &&
                        read_register(REG_PERCENT, &percent) &&
                        (status1 & 0x08u) && percent <= 100;
        if (ok) {
            s_level.store(static_cast<float>(percent) / 100.0f, std::memory_order_relaxed);
            s_charging.store(((status2 >> 5) & 0x07u) == 0x01u, std::memory_order_relaxed);
        }
        s_valid.store(ok, std::memory_order_release);
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}
}  // namespace

bool battery_monitor_begin(i2c_master_bus_handle_t bus) {
    if (!bus || s_device) return s_device != nullptr;
    if (i2c_master_probe(bus, AXP2101_ADDRESS, 50) != ESP_OK) {
        ESP_LOGW(TAG, "AXP2101 not detected");
        return false;
    }
    i2c_device_config_t config = {};
    config.dev_addr_length = I2C_ADDR_BIT_LEN_7;
    config.device_address = AXP2101_ADDRESS;
    config.scl_speed_hz = 400000;
    if (i2c_master_bus_add_device(bus, &config, &s_device) != ESP_OK) {
        ESP_LOGW(TAG, "Could not attach AXP2101 monitor");
        return false;
    }
    ESP_LOGI(TAG, "AXP2101 battery monitor ready");
    return xTaskCreate(monitor_task, "battery-monitor", 3072, nullptr, 2, nullptr) == pdPASS;
}

bool battery_monitor_read(float *level, bool *charging) {
    if (!s_valid.load(std::memory_order_acquire)) return false;
    if (level) *level = s_level.load(std::memory_order_relaxed);
    if (charging) *charging = s_charging.load(std::memory_order_relaxed);
    return true;
}
