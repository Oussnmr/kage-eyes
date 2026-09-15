#include "orientation_service.h"

#include <cmath>
#include <cstdint>

#include "bsp/esp-bsp.h"
#include "driver/i2c_master.h"
#include "esp_err.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "qmi8658.h"
#include "robot_eyes.h"

namespace {
constexpr char TAG[] = "kage-orientation";
constexpr int SAMPLE_PERIOD_MS = 50;
constexpr int PROBE_TIMEOUT_MS = 100;
constexpr float ENTER_THRESHOLD = 6.0f;  // m/s², about 0.6 g
constexpr float RELEASE_THRESHOLD = 4.0f;
constexpr int REQUIRED_SAMPLES = 4;
constexpr float SHAKE_JERK_THRESHOLD = 12.0f;
constexpr float SHAKE_GRAVITY_ERROR = 4.5f;
constexpr int64_t SHAKE_COOLDOWN_US = 2000000;
constexpr uint8_t RESET_REGISTER = 0x60;
constexpr uint8_t RESET_COMMAND = 0xB0;
constexpr uint8_t CTRL1_VALUE = 0x60;

static lv_display_t *s_display;
static qmi8658_dev_t s_imu = {};
static lv_display_rotation_t s_rotation = LV_DISPLAY_ROTATION_90;

static esp_err_t detect_address(i2c_master_bus_handle_t bus, uint8_t *address) {
    const uint8_t candidates[] = {QMI8658_ADDRESS_HIGH, QMI8658_ADDRESS_LOW};
    for (uint8_t candidate : candidates) {
        if (i2c_master_probe(bus, candidate, PROBE_TIMEOUT_MS) == ESP_OK) {
            *address = candidate;
            return ESP_OK;
        }
    }
    return ESP_ERR_NOT_FOUND;
}

static esp_err_t configure_imu(i2c_master_bus_handle_t bus) {
    uint8_t address = 0;
    esp_err_t err = detect_address(bus, &address);
    if (err != ESP_OK) return err;
    if ((err = qmi8658_init(&s_imu, bus, address)) != ESP_OK) return err;
    if ((err = qmi8658_write_register(&s_imu, RESET_REGISTER, RESET_COMMAND)) != ESP_OK) return err;
    vTaskDelay(pdMS_TO_TICKS(20));
    if ((err = qmi8658_write_register(&s_imu, QMI8658_CTRL1, CTRL1_VALUE)) != ESP_OK) return err;
    if ((err = qmi8658_set_accel_range(&s_imu, QMI8658_ACCEL_RANGE_4G)) != ESP_OK) return err;
    if ((err = qmi8658_set_accel_odr(&s_imu, QMI8658_ACCEL_ODR_250HZ)) != ESP_OK) return err;
    qmi8658_set_accel_unit_mps2(&s_imu, true);
    ESP_LOGI(TAG, "QMI8658 detected at 0x%02x", address);
    return qmi8658_enable_sensors(&s_imu, QMI8658_ENABLE_ACCEL);
}

static void orientation_task(void *) {
    lv_display_rotation_t pending = s_rotation;
    int confirmations = 0;
    bool have_previous = false;
    float previous_x = 0.0f, previous_y = 0.0f, previous_z = 0.0f;
    int64_t last_shake_us = 0;
    while (true) {
        bool ready = false;
        if (qmi8658_is_data_ready(&s_imu, &ready) == ESP_OK && ready) {
            qmi8658_data_t data = {};
            if (qmi8658_read_sensor_data(&s_imu, &data) == ESP_OK) {
                const float jerk = std::fabs(data.accelX - previous_x) +
                                   std::fabs(data.accelY - previous_y) +
                                   std::fabs(data.accelZ - previous_z);
                const float magnitude = std::sqrt(data.accelX * data.accelX +
                                                  data.accelY * data.accelY +
                                                  data.accelZ * data.accelZ);
                const int64_t now = esp_timer_get_time();
                const bool shaken = have_previous && jerk > SHAKE_JERK_THRESHOLD &&
                                    std::fabs(magnitude - 9.807f) > SHAKE_GRAVITY_ERROR &&
                                    now - last_shake_us > SHAKE_COOLDOWN_US;
                previous_x = data.accelX;
                previous_y = data.accelY;
                previous_z = data.accelZ;
                have_previous = true;

                if (shaken) {
                    last_shake_us = now;
                    pending = s_rotation;
                    confirmations = 0;
                    robot_eyes_on_shake();
                    ESP_LOGI(TAG, "Shake detected");
                    vTaskDelay(pdMS_TO_TICKS(SAMPLE_PERIOD_MS));
                    continue;
                }

                lv_display_rotation_t wanted = s_rotation;
                if (data.accelY < -ENTER_THRESHOLD) wanted = LV_DISPLAY_ROTATION_90;
                else if (data.accelY > ENTER_THRESHOLD) wanted = LV_DISPLAY_ROTATION_270;

                if (std::fabs(data.accelY) < RELEASE_THRESHOLD || wanted == s_rotation) {
                    pending = s_rotation;
                    confirmations = 0;
                } else if (wanted != pending) {
                    pending = wanted;
                    confirmations = 1;
                } else if (++confirmations >= REQUIRED_SAMPLES) {
                    if (bsp_display_lock(250)) {
                        bsp_display_rotate(s_display, pending);
                        bsp_display_unlock();
                        s_rotation = pending;
                        ESP_LOGI(TAG, "Landscape orientation: %s",
                                 s_rotation == LV_DISPLAY_ROTATION_90 ? "left" : "right");
                    }
                    confirmations = 0;
                }
            }
        }
        vTaskDelay(pdMS_TO_TICKS(SAMPLE_PERIOD_MS));
    }
}
}  // namespace

bool orientation_service_begin(lv_display_t *display) {
    if (!display) return false;
    s_display = display;
    esp_err_t err = configure_imu(bsp_i2c_get_handle());
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Auto-rotation disabled: %s", esp_err_to_name(err));
        return false;
    }
    return xTaskCreate(orientation_task, "orientation", 4096, nullptr, 3, nullptr) == pdPASS;
}
