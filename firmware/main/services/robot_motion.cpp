#include "robot_motion.h"

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstdint>

#include "bsp/esp-bsp.h"
#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "driver/ledc.h"
#include "esp_err.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs.h"

#include "event_log.h"

namespace {
constexpr char TAG[] = "robot-motion";

// Wiring reported by the user.
constexpr gpio_num_t LEFT_IN1 = GPIO_NUM_18;
constexpr gpio_num_t LEFT_IN2 = GPIO_NUM_38;
// Right side is wired IN3=GPIO42 and IN4=GPIO40.
constexpr gpio_num_t RIGHT_IN3 = GPIO_NUM_42;
constexpr gpio_num_t RIGHT_IN4 = GPIO_NUM_40;

constexpr ledc_mode_t PWM_MODE = LEDC_LOW_SPEED_MODE;
constexpr ledc_timer_t PWM_TIMER = LEDC_TIMER_1;
constexpr ledc_timer_bit_t PWM_BITS = LEDC_TIMER_10_BIT;
constexpr uint32_t PWM_MAX = (1U << 10) - 1U;
constexpr uint32_t TEST_DUTY = PWM_MAX;  // Full-power diagnostic pulse, wheels raised.
constexpr TickType_t MOTOR_PULSE = pdMS_TO_TICKS(1000);
constexpr TickType_t DRIVE_WATCHDOG = pdMS_TO_TICKS(400);

constexpr uint8_t PCA_ADDRESS = 0x40;
constexpr uint8_t PCA_MODE1 = 0x00;
constexpr uint8_t PCA_MODE2 = 0x01;
constexpr uint8_t PCA_LED0_ON_L = 0x06;
constexpr uint8_t PCA_PRESCALE = 0xFE;
constexpr uint8_t PCA_HORIZONTAL_CHANNEL = 15;
constexpr uint8_t PCA_VERTICAL_CHANNEL = 14;
constexpr uint8_t PCA_50HZ_PRESCALE = 121;
constexpr TickType_t SERVO_STEP = pdMS_TO_TICKS(800);
constexpr uint16_t SERVO_LOW_US = 1200;
constexpr uint16_t SERVO_HIGH_US = 1800;
constexpr uint16_t SERVO_CENTER_US = 1500;
constexpr uint16_t SERVO_MIN_US = 900;
constexpr uint16_t SERVO_MAX_US = 2100;
constexpr uint16_t PAN_LEFT_SAFE_US = 1100;
constexpr uint16_t PAN_RIGHT_SAFE_US = 1900;
// The vertical bracket is manually parked at this outer cable-safe end while
// unpowered. From there it may travel only toward its mechanical centre.
constexpr uint16_t TILT_OUTER_SAFE_US = 900;
constexpr uint16_t TILT_CENTRE_SAFE_US = SERVO_CENTER_US;
constexpr uint16_t SERVO_NUDGE_US = 35;
constexpr TickType_t SERVO_NUDGE_HOLD = pdMS_TO_TICKS(140);
constexpr TickType_t SERVO_SMOOTH_TICK = pdMS_TO_TICKS(20);
constexpr TickType_t SERVO_RELEASE_DELAY = pdMS_TO_TICKS(500);
constexpr uint16_t PAN_POSE_MAX_OFFSET_US = 360;   // three former 120 us clicks
constexpr uint16_t TILT_POSE_MAX_OFFSET_US = 450; // five former 90 us clicks
constexpr char MOTION_NVS_NAMESPACE[] = "kage_motion";
constexpr char PAN_ZERO_KEY[] = "pan_zero";
constexpr char TILT_ZERO_KEY[] = "tilt_zero";
constexpr char MOTOR_LIMIT_KEY[] = "motor_limit";
constexpr char PAN_RANGE_KEY[] = "pan_range";
constexpr char TILT_RANGE_KEY[] = "tilt_range";
constexpr char SERVO_SPEED_KEY[] = "servo_speed";

static std::atomic<bool> s_busy{false};
static std::atomic<bool> s_servo_cancel{false};
static std::atomic<int> s_drive_left{0};
static std::atomic<int> s_drive_right{0};
static std::atomic<uint32_t> s_drive_deadline{0};
static bool s_pwm_ready;
static bool s_i2c_diagnostics_logged;
static i2c_master_dev_handle_t s_pca;
static uint16_t s_pan_us = SERVO_CENTER_US;
// PCA9685 has no position feedback.  Match the known hand-parked position so
// the first press is a small move rather than a jump to the logical centre.
static uint16_t s_tilt_us = TILT_OUTER_SAFE_US;
static std::atomic<uint16_t> s_pan_target_us{SERVO_CENTER_US};
static std::atomic<uint16_t> s_tilt_target_us{TILT_OUTER_SAFE_US};
static std::atomic<uint16_t> s_pan_zero_us{SERVO_CENTER_US};
static std::atomic<uint16_t> s_tilt_zero_us{TILT_OUTER_SAFE_US};
static std::atomic<bool> s_calibration_mode{false};
static std::atomic<uint8_t> s_motor_limit{100};
static std::atomic<uint8_t> s_pan_range{100};
static std::atomic<uint8_t> s_tilt_range{50};
static std::atomic<uint8_t> s_servo_speed{50};
static std::atomic<int> s_behavior{ROBOT_BEHAVIOR_NEUTRAL};
static std::atomic<uint32_t> s_behavior_generation{0};
static std::atomic<uint16_t> s_behavior_duration_ms{1200};
static std::atomic<bool> s_dance_active{false};
static std::atomic<uint32_t> s_dance_started{0};
static std::atomic<bool> s_spin360_active{false};
static std::atomic<uint32_t> s_spin360_started{0};
static std::atomic<bool> s_behavior_task_started{false};

static bool pca_init();
static esp_err_t pca_channel(uint8_t channel, uint16_t pulse, bool full_off);
static bool servo_position(uint8_t channel, uint16_t micros);
static uint16_t clamp_servo(int value);

static bool valid_saved_zeros(uint16_t pan, uint16_t tilt) {
    return pan >= SERVO_MIN_US + PAN_POSE_MAX_OFFSET_US &&
           pan <= SERVO_MAX_US - PAN_POSE_MAX_OFFSET_US &&
           tilt >= SERVO_MIN_US &&
           tilt <= SERVO_MAX_US - TILT_POSE_MAX_OFFSET_US;
}

static uint8_t read_percent(nvs_handle_t handle, const char *key, uint8_t fallback) {
    uint8_t value = fallback;
    if (nvs_get_u8(handle, key, &value) != ESP_OK || value < 10 || value > 100) {
        return fallback;
    }
    return value;
}

static void load_motion_settings() {
    nvs_handle_t handle;
    if (nvs_open(MOTION_NVS_NAMESPACE, NVS_READONLY, &handle) != ESP_OK) return;
    s_motor_limit.store(read_percent(handle, MOTOR_LIMIT_KEY, 100), std::memory_order_release);
    s_pan_range.store(read_percent(handle, PAN_RANGE_KEY, 100), std::memory_order_release);
    s_tilt_range.store(read_percent(handle, TILT_RANGE_KEY, 50), std::memory_order_release);
    s_servo_speed.store(read_percent(handle, SERVO_SPEED_KEY, 50), std::memory_order_release);
    nvs_close(handle);
    event_log_add("Motion config: motor %u pan %u tilt %u speed %u%%",
                  s_motor_limit.load(), s_pan_range.load(), s_tilt_range.load(),
                  s_servo_speed.load());
}

static void load_servo_zeros() {
    nvs_handle_t handle;
    if (nvs_open(MOTION_NVS_NAMESPACE, NVS_READONLY, &handle) != ESP_OK) return;
    uint16_t pan = 0;
    uint16_t tilt = 0;
    const bool loaded = nvs_get_u16(handle, PAN_ZERO_KEY, &pan) == ESP_OK &&
                        nvs_get_u16(handle, TILT_ZERO_KEY, &tilt) == ESP_OK;
    nvs_close(handle);
    if (!loaded || !valid_saved_zeros(pan, tilt)) {
        event_log_add("Servo calibration missing or invalid");
        return;
    }
    s_pan_zero_us.store(pan, std::memory_order_release);
    s_tilt_zero_us.store(tilt, std::memory_order_release);
    s_pan_us = pan;
    s_tilt_us = tilt;
    s_pan_target_us.store(pan, std::memory_order_release);
    s_tilt_target_us.store(tilt, std::memory_order_release);
    event_log_add("Servo zeros loaded: %u/%u us", pan, tilt);
}

constexpr gpio_num_t MOTOR_PINS[] = {LEFT_IN1, LEFT_IN2, RIGHT_IN3, RIGHT_IN4};
constexpr ledc_channel_t MOTOR_CHANNELS[] = {
    LEDC_CHANNEL_0, LEDC_CHANNEL_1, LEDC_CHANNEL_2, LEDC_CHANNEL_3,
};

static void motors_stop() {
    if (!s_pwm_ready) {
        for (gpio_num_t pin : MOTOR_PINS) gpio_set_level(pin, 0);
        return;
    }
    for (ledc_channel_t channel : MOTOR_CHANNELS) {
        ledc_set_duty(PWM_MODE, channel, 0);
        ledc_update_duty(PWM_MODE, channel);
    }
}

static bool pwm_init() {
    if (s_pwm_ready) return true;

    ledc_timer_config_t timer = {};
    timer.speed_mode = PWM_MODE;
    timer.duty_resolution = PWM_BITS;
    timer.timer_num = PWM_TIMER;
    timer.freq_hz = 10000;
    timer.clk_cfg = LEDC_AUTO_CLK;
    if (ledc_timer_config(&timer) != ESP_OK) return false;

    for (size_t i = 0; i < 4; ++i) {
        ledc_channel_config_t channel = {};
        channel.gpio_num = MOTOR_PINS[i];
        channel.speed_mode = PWM_MODE;
        channel.channel = MOTOR_CHANNELS[i];
        channel.intr_type = LEDC_INTR_DISABLE;
        channel.timer_sel = PWM_TIMER;
        channel.duty = 0;
        channel.hpoint = 0;
        if (ledc_channel_config(&channel) != ESP_OK) {
            motors_stop();
            return false;
        }
    }
    s_pwm_ready = true;
    motors_stop();
    return true;
}

static void set_motor_channel(size_t index, uint32_t duty) {
    ledc_set_duty(PWM_MODE, MOTOR_CHANNELS[index], duty);
    ledc_update_duty(PWM_MODE, MOTOR_CHANNELS[index]);
}

static uint32_t percent_duty(int percent) {
    const int magnitude = percent < 0 ? -percent : percent;
    const uint32_t requested = static_cast<uint32_t>(magnitude > 100 ? 100 : magnitude);
    return requested * s_motor_limit.load(std::memory_order_acquire) * PWM_MAX / 10000U;
}

static void motors_apply_analog(int left, int right) {
    motors_stop();
    const uint32_t left_duty = percent_duty(left);
    const uint32_t right_duty = percent_duty(right);
    if (left > 0) set_motor_channel(1, left_duty);       // left OUT2
    else if (left < 0) set_motor_channel(0, left_duty);  // left OUT1
    if (right > 0) set_motor_channel(3, right_duty);     // right OUT4
    else if (right < 0) set_motor_channel(2, right_duty);// right OUT3
}

static uint16_t servo_step_us() {
    uint16_t speed = s_servo_speed.load(std::memory_order_acquire);
    // Explore keeps the full calibrated pose range but moves the head more
    // deliberately, independently of the user's global servo setting.
    if (s_behavior.load(std::memory_order_acquire) == ROBOT_BEHAVIOR_EXPLORE) {
        speed = std::min<uint16_t>(speed, 75);
    }
    // 10..100% maps to 4..28 us per 20 ms. The default 50% is 14 us,
    // matching the previously tested movement speed.
    return static_cast<uint16_t>(4U + (speed - 10U) * 24U / 90U);
}

static uint16_t step_toward(uint16_t current, uint16_t target, uint16_t step) {
    if (current < target) return static_cast<uint16_t>(
        current + step > target ? target : current + step);
    if (current > target) return static_cast<uint16_t>(
        current - step < target ? target : current - step);
    return current;
}

static void servo_smooth_task(void *) {
    TickType_t settled_since = 0;
    for (;;) {
        const uint16_t pan_target = s_pan_target_us.load(std::memory_order_acquire);
        const uint16_t tilt_target = s_tilt_target_us.load(std::memory_order_acquire);
        const bool moving = s_pan_us != pan_target || s_tilt_us != tilt_target;
        if (moving && pca_init()) {
            const uint16_t step = servo_step_us();
            const uint16_t next_pan = step_toward(s_pan_us, pan_target, step);
            const uint16_t next_tilt = step_toward(s_tilt_us, tilt_target, step);
            if (next_pan != s_pan_us) (void)servo_position(PCA_HORIZONTAL_CHANNEL, next_pan);
            if (next_tilt != s_tilt_us) (void)servo_position(PCA_VERTICAL_CHANNEL, next_tilt);
            s_pan_us = next_pan;
            s_tilt_us = next_tilt;
            settled_since = 0;
        } else if (s_pca && s_calibration_mode.load(std::memory_order_acquire)) {
            // Keep both channels energized while the user performs the fine
            // adjustment so the saved zero corresponds to a real PWM value.
            (void)servo_position(PCA_HORIZONTAL_CHANNEL, s_pan_us);
            (void)servo_position(PCA_VERTICAL_CHANNEL, s_tilt_us);
            settled_since = 0;
        } else if (s_pca) {
            const TickType_t now = xTaskGetTickCount();
            if (!settled_since) settled_since = now;
            if (now - settled_since >= SERVO_RELEASE_DELAY) {
                (void)pca_channel(PCA_HORIZONTAL_CHANNEL, 0, true);
                (void)pca_channel(PCA_VERTICAL_CHANNEL, 0, true);
            }
        }
        vTaskDelay(SERVO_SMOOTH_TICK);
    }
}

static void drive_watchdog_task(void *) {
    int applied_left = 1000;
    int applied_right = 1000;
    for (;;) {
        const uint32_t now = xTaskGetTickCount();
        int requested_left = s_drive_left.load(std::memory_order_acquire);
        int requested_right = s_drive_right.load(std::memory_order_acquire);
        if (s_spin360_active.load(std::memory_order_acquire)) {
            const uint32_t elapsed = now - s_spin360_started.load(std::memory_order_acquire);
            if (elapsed >= pdMS_TO_TICKS(2500)) {
                s_spin360_active.store(false, std::memory_order_release);
                requested_left = requested_right = 0;
                s_drive_left.store(0, std::memory_order_release);
                s_drive_right.store(0, std::memory_order_release);
                s_drive_deadline.store(0, std::memory_order_release);
            } else {
                requested_left = 100;
                requested_right = -100;
                s_drive_deadline.store(now + DRIVE_WATCHDOG, std::memory_order_release);
            }
        } else if (s_dance_active.load(std::memory_order_acquire)) {
            const uint32_t elapsed = now - s_dance_started.load(std::memory_order_acquire);
            if (elapsed >= pdMS_TO_TICKS(5000)) {
                s_dance_active.store(false, std::memory_order_release);
                requested_left = requested_right = 0;
                s_drive_left.store(0, std::memory_order_release);
                s_drive_right.store(0, std::memory_order_release);
                s_drive_deadline.store(0, std::memory_order_release);
            } else {
                const uint32_t phase = (elapsed / pdMS_TO_TICKS(360)) % 4U;
                if (phase == 0U) {
                    requested_left = 80;
                    requested_right = -80;
                } else if (phase == 1U) {
                    requested_left = 80;
                    requested_right = 80;
                } else if (phase == 2U) {
                    requested_left = -80;
                    requested_right = 80;
                } else {
                    requested_left = -80;
                    requested_right = -80;
                }
                s_drive_deadline.store(now + DRIVE_WATCHDOG, std::memory_order_release);
            }
        }
        const uint32_t deadline = s_drive_deadline.load(std::memory_order_acquire);
        const bool alive = static_cast<int32_t>(deadline - now) > 0;

        if (alive && (requested_left != applied_left || requested_right != applied_right)) {
            if (pwm_init()) {
                motors_apply_analog(requested_left, requested_right);
                applied_left = requested_left;
                applied_right = requested_right;
            } else {
                ESP_LOGE(TAG, "PWM initialization failed");
                event_log_add("Motor PWM init failed");
                s_drive_left.store(0, std::memory_order_release);
                s_drive_right.store(0, std::memory_order_release);
            }
        } else if (!alive && (applied_left || applied_right)) {
            motors_stop();
            applied_left = 0;
            applied_right = 0;
        }
        vTaskDelay(pdMS_TO_TICKS(20));
    }
}

static void set_behavior_pose(int pan_percent, int tilt_percent) {
    pan_percent = std::max(-100, std::min(100, pan_percent));
    tilt_percent = std::max(0, std::min(100, tilt_percent));
    const int pan_offset = PAN_POSE_MAX_OFFSET_US *
                           s_pan_range.load(std::memory_order_acquire) / 100;
    const int tilt_offset = TILT_POSE_MAX_OFFSET_US *
                            s_tilt_range.load(std::memory_order_acquire) / 100;
    const int pan = static_cast<int>(s_pan_zero_us.load(std::memory_order_acquire)) +
                    pan_offset * pan_percent / 100;
    const int tilt = static_cast<int>(s_tilt_zero_us.load(std::memory_order_acquire)) +
                     tilt_offset * tilt_percent / 100;
    s_pan_target_us.store(clamp_servo(pan), std::memory_order_release);
    s_tilt_target_us.store(clamp_servo(tilt), std::memory_order_release);
}

static void behavior_task(void *) {
    uint32_t seen_generation = 0;
    uint32_t started = 0;
    int active_behavior = ROBOT_BEHAVIOR_NEUTRAL;
    int applied_pan = 1000;
    int applied_tilt = 1000;
    for (;;) {
        const uint32_t generation = s_behavior_generation.load(std::memory_order_acquire);
        if (generation != seen_generation) {
            seen_generation = generation;
            active_behavior = s_behavior.load(std::memory_order_acquire);
            started = xTaskGetTickCount();
            applied_pan = applied_tilt = 1000;
        }
        if (active_behavior != ROBOT_BEHAVIOR_NEUTRAL &&
            !s_calibration_mode.load(std::memory_order_acquire)) {
            const uint32_t elapsed_ms = (xTaskGetTickCount() - started) * 1000U / configTICK_RATE_HZ;
            const uint32_t duration = s_behavior_duration_ms.load(std::memory_order_acquire);
            if (elapsed_ms >= duration) {
                active_behavior = ROBOT_BEHAVIOR_NEUTRAL;
                s_behavior.store(ROBOT_BEHAVIOR_NEUTRAL, std::memory_order_release);
                if (applied_pan != 0 || applied_tilt != 0) {
                    set_behavior_pose(0, 0);
                    applied_pan = applied_tilt = 0;
                }
            } else {
                int pan = 0;
                int tilt = 0;
                switch (active_behavior) {
                    case ROBOT_BEHAVIOR_AFFIRM:
                        tilt = (elapsed_ms % 900U) < 240U ? 35 : 0;
                        break;
                    case ROBOT_BEHAVIOR_LISTENING:
                        tilt = 25;
                        break;
                    case ROBOT_BEHAVIOR_DENY: {
                        const uint32_t phase = (elapsed_ms / 300U) % 3U;
                        pan = phase == 0 ? -45 : (phase == 1 ? 45 : 0);
                        break;
                    }
                    case ROBOT_BEHAVIOR_THINKING:
                    case ROBOT_BEHAVIOR_SARCASTIC:
                        if (elapsed_ms < 750U) pan = -35;
                        break;
                    case ROBOT_BEHAVIOR_AMUSED:
                    case ROBOT_BEHAVIOR_HAPPY:
                    case ROBOT_BEHAVIOR_CELEBRATE:
                    case ROBOT_BEHAVIOR_SATISFIED:
                    case ROBOT_BEHAVIOR_GENTLE:
                        tilt = (elapsed_ms % 1200U) < 260U ? 30 : 0;
                        break;
                    case ROBOT_BEHAVIOR_CURIOUS:
                    case ROBOT_BEHAVIOR_SURPRISED:
                        if (elapsed_ms < 650U) pan = 35;
                        break;
                    case ROBOT_BEHAVIOR_CONFUSED:
                        pan = ((elapsed_ms / 450U) % 2U) ? 35 : -35;
                        break;
                    case ROBOT_BEHAVIOR_WORRIED:
                    case ROBOT_BEHAVIOR_WARNING:
                        tilt = 30;
                        break;
                    case ROBOT_BEHAVIOR_SAD:
                        pan = -30;
                        break;
                    case ROBOT_BEHAVIOR_EXPLORE: {
                        // One axis at a time, with a centre pause between poses.
                        // This avoids the simultaneous full-range MG90 current
                        // peaks that can brown out Wi-Fi during exploration.
                        const uint32_t phase = (elapsed_ms / 1250U) % 8U;
                        // Alternate a stopped head scan with a short, bounded
                        // track movement. Never drive tracks and servos in the
                        // same phase, so the robot appears to search naturally.
                        if (phase == 0) pan = -100;
                        else if (phase == 1) pan = 0;
                        else if (phase == 2) {
                            s_drive_left.store(70, std::memory_order_release);
                            s_drive_right.store(70, std::memory_order_release);
                            s_drive_deadline.store(xTaskGetTickCount() + pdMS_TO_TICKS(1100), std::memory_order_release);
                        } else if (phase == 4) tilt = 100;
                        else if (phase == 5) tilt = 0;
                        else if (phase == 6) {
                            s_drive_left.store(-65, std::memory_order_release);
                            s_drive_right.store(65, std::memory_order_release);
                            s_drive_deadline.store(xTaskGetTickCount() + pdMS_TO_TICKS(1100), std::memory_order_release);
                        }
                        break;
                    }
                    default:
                        break;
                }
                // Unlike the old 50 Hz loop, publish only when the pose changes.
                if (pan != applied_pan || tilt != applied_tilt) {
                    set_behavior_pose(pan, tilt);
                    applied_pan = pan;
                    applied_tilt = tilt;
                }
            }
        }
        vTaskDelay(pdMS_TO_TICKS(40));
    }
}

static bool ensure_behavior_task() {
    bool expected = false;
    if (!s_behavior_task_started.compare_exchange_strong(
            expected, true, std::memory_order_acq_rel)) {
        return true;
    }
    if (xTaskCreate(behavior_task, "robot_behavior", 3072, nullptr, 4, nullptr) != pdPASS) {
        s_behavior_task_started.store(false, std::memory_order_release);
        ESP_LOGE(TAG, "Behavior task creation failed");
        event_log_add("Behavior task unavailable");
        return false;
    }
    return true;
}

static void motors_pulse(bool forward) {
    if (!pwm_init()) {
        ESP_LOGE(TAG, "PWM initialization failed");
        event_log_add("Motor PWM init failed");
        return;
    }

    motors_stop();
    if (forward) {
        // OUT2 and OUT4 are the user-marked positive motor leads.
        set_motor_channel(1, TEST_DUTY);
        set_motor_channel(3, TEST_DUTY);
    } else {
        set_motor_channel(0, TEST_DUTY);
        set_motor_channel(2, TEST_DUTY);
    }
    vTaskDelay(MOTOR_PULSE);
    motors_stop();
}

static esp_err_t pca_write(uint8_t reg, uint8_t value) {
    const uint8_t bytes[2] = {reg, value};
    return i2c_master_transmit(s_pca, bytes, sizeof(bytes), 100);
}

static esp_err_t pca_channel(uint8_t channel, uint16_t pulse, bool full_off) {
    const uint8_t reg = static_cast<uint8_t>(PCA_LED0_ON_L + 4U * channel);
    const uint8_t bytes[5] = {
        reg,
        0,
        0,
        static_cast<uint8_t>(pulse & 0xFF),
        static_cast<uint8_t>(full_off ? 0x10 : ((pulse >> 8) & 0x0F)),
    };
    return i2c_master_transmit(s_pca, bytes, sizeof(bytes), 100);
}

static void log_i2c_devices_once() {
    if (s_i2c_diagnostics_logged) return;
    s_i2c_diagnostics_logged = true;

    i2c_master_bus_handle_t bus = bsp_i2c_get_handle();
    unsigned found = 0;
    for (uint8_t address = 0x08; address <= 0x77; ++address) {
        if (i2c_master_probe(bus, address, 30) == ESP_OK) {
            ESP_LOGI(TAG, "I2C device detected at 0x%02x", address);
            event_log_add("I2C device: 0x%02X", address);
            ++found;
        }
    }
    ESP_LOGI(TAG, "I2C scan complete: %u device(s)", found);
    event_log_add("I2C scan: %u device(s)", found);
}

static bool pca_init() {
    if (s_pca) return true;

    log_i2c_devices_once();
    if (i2c_master_probe(bsp_i2c_get_handle(), PCA_ADDRESS, 100) != ESP_OK) {
        ESP_LOGE(TAG, "PCA9685 not detected at 0x%02x", PCA_ADDRESS);
        event_log_add("PCA9685 0x40 not detected");
        return false;
    }

    i2c_device_config_t config = {};
    config.dev_addr_length = I2C_ADDR_BIT_LEN_7;
    config.device_address = PCA_ADDRESS;
    config.scl_speed_hz = 100000;
    if (i2c_master_bus_add_device(bsp_i2c_get_handle(), &config, &s_pca) != ESP_OK) {
        s_pca = nullptr;
        return false;
    }

    // Configure 50 Hz while asleep, then wake with auto-increment enabled.
    if (pca_write(PCA_MODE1, 0x10) != ESP_OK ||
        pca_write(PCA_PRESCALE, PCA_50HZ_PRESCALE) != ESP_OK ||
        pca_write(PCA_MODE2, 0x04) != ESP_OK ||
        pca_write(PCA_MODE1, 0x20) != ESP_OK) {
        i2c_master_bus_rm_device(s_pca);
        s_pca = nullptr;
        return false;
    }
    vTaskDelay(pdMS_TO_TICKS(2));

    // No startup movement: explicitly disable every PCA output.  A channel is
    // enabled only for its short test pulse and is disabled again afterward.
    for (uint8_t channel = 0; channel < 16; ++channel) {
        if (pca_channel(channel, 0, true) != ESP_OK) {
            i2c_master_bus_rm_device(s_pca);
            s_pca = nullptr;
            return false;
        }
    }
    ESP_LOGI(TAG, "PCA9685 ready at 0x%02x", PCA_ADDRESS);
    event_log_add("PCA9685 0x40 ready");
    return true;
}

static uint16_t micros_to_ticks(uint16_t micros) {
    return static_cast<uint16_t>((static_cast<uint32_t>(micros) * 4096U) / 20000U);
}

static bool servo_position(uint8_t channel, uint16_t micros) {
    if (pca_channel(channel, micros_to_ticks(micros), false) == ESP_OK) return true;
    ESP_LOGE(TAG, "PCA9685 channel %u write failed", channel);
    event_log_add("Servo channel %u failed", channel);
    return false;
}

static void servo_sweep(uint8_t channel) {
    if (!pca_init()) {
        ESP_LOGE(TAG, "PCA9685 not available at 0x%02x", PCA_ADDRESS);
        event_log_add("PCA9685 unavailable");
        return;
    }

    event_log_add("Servo %u sweep: 1200-1800-1500", channel);
    if (!servo_position(channel, SERVO_LOW_US)) return;
    vTaskDelay(SERVO_STEP);
    if (!servo_position(channel, SERVO_HIGH_US)) return;
    vTaskDelay(SERVO_STEP);
    if (!servo_position(channel, SERVO_CENTER_US)) return;
    vTaskDelay(SERVO_STEP);
    (void)pca_channel(channel, 0, true);
}

static uint16_t clamp_servo(int value) {
    if (value < SERVO_MIN_US) return SERVO_MIN_US;
    if (value > SERVO_MAX_US) return SERVO_MAX_US;
    return static_cast<uint16_t>(value);
}

static uint16_t clamp_tilt(int value) {
    if (value < TILT_OUTER_SAFE_US) return TILT_OUTER_SAFE_US;
    if (value > TILT_CENTRE_SAFE_US) return TILT_CENTRE_SAFE_US;
    return static_cast<uint16_t>(value);
}

static void servo_nudge_task(void *argument) {
    const RobotServoNudge nudge =
        static_cast<RobotServoNudge>(reinterpret_cast<uintptr_t>(argument));
    uint8_t channel = PCA_HORIZONTAL_CHANNEL;
    uint16_t *target = &s_pan_us;
    int delta = 0;
    switch (nudge) {
        case ROBOT_PAN_LEFT: delta = -SERVO_NUDGE_US; break;
        case ROBOT_PAN_RIGHT: delta = SERVO_NUDGE_US; break;
        case ROBOT_TILT_UP:
            channel = PCA_VERTICAL_CHANNEL;
            target = &s_tilt_us;
            delta = SERVO_NUDGE_US;
            break;
        case ROBOT_TILT_DOWN:
            channel = PCA_VERTICAL_CHANNEL;
            target = &s_tilt_us;
            delta = -SERVO_NUDGE_US;
            break;
    }

    if (pca_init()) {
        *target = channel == PCA_VERTICAL_CHANNEL
            ? clamp_tilt(static_cast<int>(*target) + delta)
            : clamp_servo(static_cast<int>(*target) + delta);
        if (servo_position(channel, *target)) {
            event_log_add("Servo %u: %u us", channel, *target);
            TickType_t remaining = SERVO_NUDGE_HOLD;
            while (remaining > 0 && !s_servo_cancel.load(std::memory_order_acquire)) {
                const TickType_t slice = remaining > pdMS_TO_TICKS(20)
                    ? pdMS_TO_TICKS(20) : remaining;
                vTaskDelay(slice);
                remaining -= slice;
            }
            (void)pca_channel(channel, 0, true);
        }
    } else {
        event_log_add("PCA9685 unavailable");
    }
    s_busy.store(false);
    vTaskDelete(nullptr);
}

static void motion_task(void *argument) {
    const RobotMove move = static_cast<RobotMove>(reinterpret_cast<uintptr_t>(argument));
    switch (move) {
        case ROBOT_MOVE_HORIZONTAL:
            event_log_add("Test: horizontal servo");
            servo_sweep(PCA_HORIZONTAL_CHANNEL);
            break;
        case ROBOT_MOVE_VERTICAL:
            event_log_add("Test: vertical servo");
            servo_sweep(PCA_VERTICAL_CHANNEL);
            break;
        case ROBOT_MOVE_FORWARD:
            event_log_add("Test: motors forward");
            motors_pulse(true);
            break;
        case ROBOT_MOVE_BACKWARD:
            event_log_add("Test: motors backward");
            motors_pulse(false);
            break;
    }
    motors_stop();
    s_busy.store(false);
    vTaskDelete(nullptr);
}
}  // namespace

void robot_motion_safe_boot(void) {
    for (gpio_num_t pin : MOTOR_PINS) {
        gpio_reset_pin(pin);
        gpio_set_pull_mode(pin, GPIO_PULLDOWN_ONLY);
        gpio_set_direction(pin, GPIO_MODE_OUTPUT);
        gpio_set_level(pin, 0);
    }
    xTaskCreate(drive_watchdog_task, "drive_watchdog", 3072, nullptr, 6, nullptr);
}

void robot_motion_begin(void) {
    load_motion_settings();
    load_servo_zeros();
    xTaskCreate(servo_smooth_task, "servo_smooth", 4096, nullptr, 5, nullptr);
}

void robot_motion_command(RobotMove move) {
    bool expected = false;
    if (!s_busy.compare_exchange_strong(expected, true)) {
        ESP_LOGW(TAG, "Motion command ignored while another test is active");
        event_log_add("Motion busy: command ignored");
        return;
    }
    if (xTaskCreate(motion_task, "motion_test", 4096,
                    reinterpret_cast<void *>(static_cast<uintptr_t>(move)), 5, nullptr) != pdPASS) {
        motors_stop();
        s_busy.store(false);
        ESP_LOGE(TAG, "Unable to create motion task");
        event_log_add("Motion task start failed");
    }
}

void robot_drive_hold(RobotDrive drive) {
    switch (drive) {
        case ROBOT_DRIVE_FORWARD: robot_drive_analog(100, 100); break;
        case ROBOT_DRIVE_BACKWARD: robot_drive_analog(-100, -100); break;
        case ROBOT_DRIVE_LEFT: robot_drive_analog(-100, 100); break;
        case ROBOT_DRIVE_RIGHT: robot_drive_analog(100, -100); break;
    }
}

void robot_drive_analog(int left_percent, int right_percent) {
    if (left_percent > 100) left_percent = 100;
    if (left_percent < -100) left_percent = -100;
    if (right_percent > 100) right_percent = 100;
    if (right_percent < -100) right_percent = -100;
    s_dance_active.store(false, std::memory_order_release);
    s_behavior.store(ROBOT_BEHAVIOR_NEUTRAL, std::memory_order_release);
    s_behavior_generation.fetch_add(1, std::memory_order_acq_rel);
    s_drive_left.store(left_percent, std::memory_order_release);
    s_drive_right.store(right_percent, std::memory_order_release);
    s_drive_deadline.store(xTaskGetTickCount() + DRIVE_WATCHDOG, std::memory_order_release);
}

void robot_motion_stop(void) {
    s_dance_active.store(false, std::memory_order_release);
    robot_motion_cancel_behavior();
    s_drive_left.store(0, std::memory_order_release);
    s_drive_right.store(0, std::memory_order_release);
    s_drive_deadline.store(0, std::memory_order_release);
    s_servo_cancel.store(true, std::memory_order_release);
    motors_stop();
}

void robot_motion_cancel_behavior(void) {
    s_behavior.store(ROBOT_BEHAVIOR_NEUTRAL, std::memory_order_release);
    s_behavior_generation.fetch_add(1, std::memory_order_acq_rel);
}

void robot_motion_behavior(int behavior, int duration_ms) {
    if (behavior <= ROBOT_BEHAVIOR_NEUTRAL || behavior > ROBOT_BEHAVIOR_EXPLORE) return;
    if (!ensure_behavior_task()) return;
    if (duration_ms < 100) duration_ms = 100;
    if (duration_ms > 3000) duration_ms = 3000;
    s_behavior_duration_ms.store(static_cast<uint16_t>(duration_ms), std::memory_order_release);
    s_behavior.store(behavior, std::memory_order_release);
    s_behavior_generation.fetch_add(1, std::memory_order_acq_rel);
}

void robot_motion_start_dance(void) {
    if (s_calibration_mode.load(std::memory_order_acquire)) return;
    s_behavior.store(ROBOT_BEHAVIOR_NEUTRAL, std::memory_order_release);
    s_behavior_generation.fetch_add(1, std::memory_order_acq_rel);
    s_dance_started.store(xTaskGetTickCount(), std::memory_order_release);
    s_dance_active.store(true, std::memory_order_release);
}

void robot_motion_start_360(void) {
    if (s_calibration_mode.load(std::memory_order_acquire)) return;
    s_dance_active.store(false, std::memory_order_release);
    s_behavior.store(ROBOT_BEHAVIOR_NEUTRAL, std::memory_order_release);
    s_behavior_generation.fetch_add(1, std::memory_order_acq_rel);
    s_spin360_started.store(xTaskGetTickCount(), std::memory_order_release);
    s_spin360_active.store(true, std::memory_order_release);
}

void robot_motion_stop_dance(void) {
    const bool was_dancing = s_dance_active.exchange(false, std::memory_order_acq_rel);
    const bool was_spinning = s_spin360_active.exchange(false, std::memory_order_acq_rel);
    if (!was_dancing && !was_spinning) return;
    s_drive_left.store(0, std::memory_order_release);
    s_drive_right.store(0, std::memory_order_release);
    s_drive_deadline.store(0, std::memory_order_release);
    motors_stop();
}

void robot_servo_targets(int pan_percent, int tilt_percent) {
    robot_motion_cancel_behavior();
    if (pan_percent < 0) pan_percent = 0;
    if (pan_percent > 100) pan_percent = 100;
    if (tilt_percent < 0) tilt_percent = 0;
    if (tilt_percent > 100) tilt_percent = 100;
    const uint16_t pan = static_cast<uint16_t>(PAN_LEFT_SAFE_US +
        (PAN_RIGHT_SAFE_US - PAN_LEFT_SAFE_US) * pan_percent / 100);
    const uint16_t tilt = static_cast<uint16_t>(TILT_OUTER_SAFE_US +
        (TILT_CENTRE_SAFE_US - TILT_OUTER_SAFE_US) * tilt_percent / 100);
    s_pan_target_us.store(pan, std::memory_order_release);
    s_tilt_target_us.store(tilt, std::memory_order_release);
}

void robot_servo_pose(int pan_state, int tilt_state) {
    if (s_calibration_mode.load(std::memory_order_acquire)) return;
    if (pan_state < -1) pan_state = -1;
    if (pan_state > 1) pan_state = 1;
    if (tilt_state < 0) tilt_state = 0;
    if (tilt_state > 1) tilt_state = 1;
    const int pan_offset = PAN_POSE_MAX_OFFSET_US *
                           s_pan_range.load(std::memory_order_acquire) / 100;
    const int tilt_offset = TILT_POSE_MAX_OFFSET_US *
                            s_tilt_range.load(std::memory_order_acquire) / 100;
    const int pan = static_cast<int>(s_pan_zero_us.load(std::memory_order_acquire)) +
                    pan_state * pan_offset;
    const int tilt = static_cast<int>(s_tilt_zero_us.load(std::memory_order_acquire)) +
                     tilt_state * tilt_offset;
    s_pan_target_us.store(clamp_servo(pan), std::memory_order_release);
    s_tilt_target_us.store(clamp_servo(tilt), std::memory_order_release);
}

void robot_servo_calibration_begin(void) {
    // Energize the last known logical position. The user can then use the
    // fine-adjust arrows to place the mechanism precisely before saving.
    s_calibration_mode.store(true, std::memory_order_release);
    s_pan_target_us.store(s_pan_us, std::memory_order_release);
    s_tilt_target_us.store(s_tilt_us, std::memory_order_release);
    if (pca_init()) {
        (void)servo_position(PCA_HORIZONTAL_CHANNEL, s_pan_us);
        (void)servo_position(PCA_VERTICAL_CHANNEL, s_tilt_us);
    }
    event_log_add("Servo calibration mode");
}

void robot_servo_adjust(int pan_delta_us, int tilt_delta_us) {
    if (!s_calibration_mode.load(std::memory_order_acquire)) return;
    int pan = static_cast<int>(s_pan_target_us.load(std::memory_order_acquire)) + pan_delta_us;
    int tilt = static_cast<int>(s_tilt_target_us.load(std::memory_order_acquire)) + tilt_delta_us;
    // Keep enough absolute headroom for every future -1/0/+1 pan pose and
    // the complete zero/up tilt motion. A saved calibration is therefore
    // always usable and can never authorize an out-of-range pulse.
    if (pan < SERVO_MIN_US + PAN_POSE_MAX_OFFSET_US) pan = SERVO_MIN_US + PAN_POSE_MAX_OFFSET_US;
    if (pan > SERVO_MAX_US - PAN_POSE_MAX_OFFSET_US) pan = SERVO_MAX_US - PAN_POSE_MAX_OFFSET_US;
    if (tilt < SERVO_MIN_US) tilt = SERVO_MIN_US;
    if (tilt > SERVO_MAX_US - TILT_POSE_MAX_OFFSET_US) tilt = SERVO_MAX_US - TILT_POSE_MAX_OFFSET_US;
    s_pan_target_us.store(static_cast<uint16_t>(pan), std::memory_order_release);
    s_tilt_target_us.store(static_cast<uint16_t>(tilt), std::memory_order_release);
}

bool robot_servo_calibration_save(void) {
    if (!s_calibration_mode.load(std::memory_order_acquire)) return false;
    const uint16_t pan = s_pan_us;
    const uint16_t tilt = s_tilt_us;
    if (!valid_saved_zeros(pan, tilt)) {
        event_log_add("Calibration rejected: unsafe zero");
        return false;
    }
    nvs_handle_t handle;
    if (nvs_open(MOTION_NVS_NAMESPACE, NVS_READWRITE, &handle) != ESP_OK) return false;
    const bool written = nvs_set_u16(handle, PAN_ZERO_KEY, pan) == ESP_OK &&
                         nvs_set_u16(handle, TILT_ZERO_KEY, tilt) == ESP_OK &&
                         nvs_commit(handle) == ESP_OK;
    nvs_close(handle);
    if (!written) return false;
    s_pan_zero_us.store(pan, std::memory_order_release);
    s_tilt_zero_us.store(tilt, std::memory_order_release);
    s_calibration_mode.store(false, std::memory_order_release);
    event_log_add("Servo zeros saved: %u/%u us", pan, tilt);
    return true;
}

bool robot_motion_settings(int motor_limit, int pan_range, int tilt_range,
                           int servo_speed) {
    if (motor_limit < 10 || motor_limit > 100 ||
        pan_range < 10 || pan_range > 100 ||
        tilt_range < 10 || tilt_range > 100 ||
        servo_speed < 10 || servo_speed > 100) {
        event_log_add("Motion config rejected");
        return false;
    }

    nvs_handle_t handle;
    if (nvs_open(MOTION_NVS_NAMESPACE, NVS_READWRITE, &handle) != ESP_OK) return false;
    const bool written = nvs_set_u8(handle, MOTOR_LIMIT_KEY, static_cast<uint8_t>(motor_limit)) == ESP_OK &&
                         nvs_set_u8(handle, PAN_RANGE_KEY, static_cast<uint8_t>(pan_range)) == ESP_OK &&
                         nvs_set_u8(handle, TILT_RANGE_KEY, static_cast<uint8_t>(tilt_range)) == ESP_OK &&
                         nvs_set_u8(handle, SERVO_SPEED_KEY, static_cast<uint8_t>(servo_speed)) == ESP_OK &&
                         nvs_commit(handle) == ESP_OK;
    nvs_close(handle);
    if (!written) return false;

    s_motor_limit.store(static_cast<uint8_t>(motor_limit), std::memory_order_release);
    s_pan_range.store(static_cast<uint8_t>(pan_range), std::memory_order_release);
    s_tilt_range.store(static_cast<uint8_t>(tilt_range), std::memory_order_release);
    s_servo_speed.store(static_cast<uint8_t>(servo_speed), std::memory_order_release);
    s_drive_left.store(0, std::memory_order_release);
    s_drive_right.store(0, std::memory_order_release);
    s_drive_deadline.store(0, std::memory_order_release);
    motors_stop();
    event_log_add("Motion config saved: %d/%d/%d/%d", motor_limit, pan_range,
                  tilt_range, servo_speed);
    return true;
}

void robot_servo_nudge(RobotServoNudge nudge) {
    bool expected = false;
    if (!s_busy.compare_exchange_strong(expected, true)) return;
    s_servo_cancel.store(false, std::memory_order_release);
    if (xTaskCreate(servo_nudge_task, "servo_nudge", 4096,
                    reinterpret_cast<void *>(static_cast<uintptr_t>(nudge)), 5, nullptr) != pdPASS) {
        s_busy.store(false);
        event_log_add("Servo nudge task failed");
    }
}
