#include "robot_motion.h"

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
constexpr TickType_t DRIVE_WATCHDOG = pdMS_TO_TICKS(850);

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
constexpr uint16_t SERVO_NUDGE_US = 75;
constexpr TickType_t SERVO_NUDGE_HOLD = pdMS_TO_TICKS(300);

static std::atomic<bool> s_busy{false};
static std::atomic<bool> s_servo_cancel{false};
static std::atomic<int> s_drive_mode{-1};
static std::atomic<uint32_t> s_drive_deadline{0};
static bool s_pwm_ready;
static bool s_i2c_diagnostics_logged;
static i2c_master_dev_handle_t s_pca;
static uint16_t s_pan_us = SERVO_CENTER_US;
static uint16_t s_tilt_us = SERVO_CENTER_US;

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

static void motors_apply(RobotDrive drive) {
    motors_stop();
    switch (drive) {
        case ROBOT_DRIVE_FORWARD:
            set_motor_channel(1, TEST_DUTY);  // left OUT2
            set_motor_channel(3, TEST_DUTY);  // right OUT4
            break;
        case ROBOT_DRIVE_BACKWARD:
            set_motor_channel(0, TEST_DUTY);  // left OUT1
            set_motor_channel(2, TEST_DUTY);  // right OUT3
            break;
        case ROBOT_DRIVE_LEFT:
            set_motor_channel(0, TEST_DUTY);  // left backward
            set_motor_channel(3, TEST_DUTY);  // right forward
            break;
        case ROBOT_DRIVE_RIGHT:
            set_motor_channel(1, TEST_DUTY);  // left forward
            set_motor_channel(2, TEST_DUTY);  // right backward
            break;
    }
}

static void drive_watchdog_task(void *) {
    int applied = -1;
    for (;;) {
        const int requested = s_drive_mode.load(std::memory_order_acquire);
        const uint32_t now = xTaskGetTickCount();
        const uint32_t deadline = s_drive_deadline.load(std::memory_order_acquire);
        const bool alive = requested >= 0 && static_cast<int32_t>(deadline - now) > 0;

        if (alive && requested != applied) {
            if (pwm_init()) {
                motors_apply(static_cast<RobotDrive>(requested));
                applied = requested;
            } else {
                ESP_LOGE(TAG, "PWM initialization failed");
                event_log_add("Motor PWM init failed");
                s_drive_mode.store(-1, std::memory_order_release);
            }
        } else if (!alive && applied != -1) {
            motors_stop();
            s_drive_mode.store(-1, std::memory_order_release);
            applied = -1;
        }
        vTaskDelay(pdMS_TO_TICKS(20));
    }
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
        *target = clamp_servo(static_cast<int>(*target) + delta);
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
    const uint32_t deadline = xTaskGetTickCount() + DRIVE_WATCHDOG;
    s_drive_deadline.store(deadline, std::memory_order_release);
    s_drive_mode.store(static_cast<int>(drive), std::memory_order_release);
}

void robot_motion_stop(void) {
    s_drive_mode.store(-1, std::memory_order_release);
    s_servo_cancel.store(true, std::memory_order_release);
    motors_stop();
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
