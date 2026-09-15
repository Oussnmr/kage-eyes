#include "robot_eyes.h"

#include <atomic>
#include <cmath>
#include <cstdint>

#include "esp_random.h"
#include "esp_timer.h"

namespace {
constexpr int SCREEN_W = 448;
constexpr int SCREEN_H = 368;
constexpr int EYE_W = 132;
constexpr int EYE_H = 132;
constexpr int EYE_OFFSET_X = 112;
constexpr int EYE_RADIUS = 32;
constexpr int FRAME_MS = 33;
constexpr float IDLE_BEFORE_SLEEP_S = 18.0f;
constexpr float SLEEP_DURATION_S = 8.0f;
constexpr float SLEEP_TRANSITION_S = 0.8f;
constexpr float WAKE_TRANSITION_S = 0.55f;
constexpr float DIZZY_DURATION_S = 2.8f;
constexpr uint32_t CYAN = 0x4FE3FF;
constexpr uint32_t CYAN_DARK = 0x16798A;

static lv_obj_t *s_left_eye;
static lv_obj_t *s_right_eye;
static int64_t s_last_frame_us;
static float s_time;
static float s_next_blink = 2.0f;
static float s_blink_time = -1.0f;
static float s_next_gaze = 1.0f;
static float s_gaze_x;
static float s_gaze_y;
static float s_target_x;
static float s_target_y;
static float s_last_activity;
static float s_sleep_started = -1.0f;
static float s_wake_started;
static float s_dizzy_until;
static std::atomic<bool> s_shake_pending{false};

static float random_unit() {
    return static_cast<float>(esp_random()) / static_cast<float>(UINT32_MAX);
}

static float approach(float current, float target, float amount) {
    return current + (target - current) * amount;
}

static float smoothstep(float value) {
    if (value < 0.0f) value = 0.0f;
    if (value > 1.0f) value = 1.0f;
    return value * value * (3.0f - 2.0f * value);
}

static void wake_up(bool blink_if_awake) {
    const bool was_sleeping = s_sleep_started >= 0.0f;
    s_sleep_started = -1.0f;
    s_last_activity = s_time;
    s_wake_started = s_time;
    s_target_x = 0.0f;
    s_target_y = 0.0f;
    if (!was_sleeping && blink_if_awake) s_blink_time = 0.0f;
}

static void touch_event(lv_event_t *) {
    wake_up(true);
}

static void set_geometry(lv_obj_t *eye, int x, int y, int width, int height) {
    if (lv_obj_get_x(eye) != x || lv_obj_get_y(eye) != y) lv_obj_set_pos(eye, x, y);
    if (lv_obj_get_width(eye) != width || lv_obj_get_height(eye) != height) {
        lv_obj_set_size(eye, width, height);
    }
}

static void animate(lv_timer_t *) {
    const int64_t now = esp_timer_get_time();
    if (now - s_last_frame_us < FRAME_MS * 1000) return;

    float dt = static_cast<float>(now - s_last_frame_us) / 1000000.0f;
    s_last_frame_us = now;
    if (dt > 0.08f) dt = 0.08f;
    s_time += dt;

    if (s_shake_pending.exchange(false)) {
        wake_up(false);
        s_dizzy_until = s_time + DIZZY_DURATION_S;
    }

    if (s_sleep_started < 0.0f && s_time >= s_dizzy_until &&
        s_time - s_last_activity >= IDLE_BEFORE_SLEEP_S) {
        s_sleep_started = s_time;
        s_target_x = 0.0f;
        s_target_y = 0.0f;
    }
    if (s_sleep_started >= 0.0f && s_time - s_sleep_started >= SLEEP_DURATION_S) {
        wake_up(false);
    }

    if (s_sleep_started < 0.0f && s_time >= s_dizzy_until) {
        s_next_gaze -= dt;
        if (s_next_gaze <= 0.0f) {
            s_next_gaze = 0.9f + random_unit() * 2.3f;
            s_target_x = (random_unit() * 2.0f - 1.0f) * 10.0f;
            s_target_y = (random_unit() * 2.0f - 1.0f) * 6.0f;
        }
    }
    const float follow = 1.0f - expf(-dt * 8.0f);
    s_gaze_x = approach(s_gaze_x, s_target_x, follow);
    s_gaze_y = approach(s_gaze_y, s_target_y, follow);

    float closure = 0.0f;
    if (s_blink_time >= 0.0f) {
        s_blink_time += dt;
        constexpr float BLINK_DURATION = 0.24f;
        const float phase = s_blink_time / BLINK_DURATION;
        if (phase >= 1.0f) {
            s_blink_time = -1.0f;
            s_next_blink = 2.0f + random_unit() * 3.5f;
        } else {
            closure = sinf(phase * 3.14159265f);
        }
    } else {
        s_next_blink -= dt;
        if (s_next_blink <= 0.0f) s_blink_time = 0.0f;
    }

    float open = smoothstep((s_time - s_wake_started) / WAKE_TRANSITION_S);
    if (s_sleep_started >= 0.0f) {
        const float closing = smoothstep((s_time - s_sleep_started) / SLEEP_TRANSITION_S);
        open *= 1.0f - closing * 0.91f;
        closure = 0.0f;
    }

    int gaze_x = static_cast<int>(s_gaze_x);
    int gaze_y = static_cast<int>(s_gaze_y);
    int left_height = static_cast<int>(4.0f + (EYE_H - 4.0f) * open * (1.0f - closure * 0.96f));
    int right_height = left_height;

    if (s_time < s_dizzy_until) {
        const float dizzy_phase = (DIZZY_DURATION_S - (s_dizzy_until - s_time)) * 11.0f;
        gaze_x = static_cast<int>(sinf(dizzy_phase) * 16.0f);
        gaze_y = static_cast<int>(cosf(dizzy_phase * 0.83f) * 10.0f);
        left_height = static_cast<int>(EYE_H * (0.72f + 0.18f * sinf(dizzy_phase * 1.31f)));
        right_height = static_cast<int>(EYE_H * (0.72f - 0.18f * sinf(dizzy_phase * 1.31f)));
    }

    const int center_y = SCREEN_H / 2 + static_cast<int>(sinf(s_time * 1.1f) * 2.0f) + gaze_y;
    const int left_center_x = SCREEN_W / 2 - EYE_OFFSET_X + gaze_x;
    const int right_center_x = SCREEN_W / 2 + EYE_OFFSET_X + gaze_x;

    set_geometry(s_left_eye, left_center_x - EYE_W / 2, center_y - left_height / 2, EYE_W, left_height);
    set_geometry(s_right_eye, right_center_x - EYE_W / 2, center_y - right_height / 2, EYE_W, right_height);
}

static lv_obj_t *create_eye(lv_obj_t *parent) {
    lv_obj_t *eye = lv_obj_create(parent);
    lv_obj_remove_style_all(eye);
    lv_obj_set_style_bg_opa(eye, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(eye, lv_color_hex(CYAN), 0);
    lv_obj_set_style_bg_grad_color(eye, lv_color_hex(CYAN_DARK), 0);
    lv_obj_set_style_bg_grad_dir(eye, LV_GRAD_DIR_VER, 0);
    lv_obj_set_style_radius(eye, EYE_RADIUS, 0);
    lv_obj_set_style_shadow_color(eye, lv_color_hex(CYAN), 0);
    lv_obj_set_style_shadow_width(eye, 14, 0);
    lv_obj_set_style_shadow_spread(eye, 2, 0);
    lv_obj_set_style_shadow_opa(eye, LV_OPA_50, 0);
    lv_obj_clear_flag(eye, LV_OBJ_FLAG_SCROLLABLE);
    return eye;
}
}  // namespace

void robot_eyes_begin(lv_obj_t *parent) {
    lv_obj_set_style_bg_color(parent, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(parent, LV_OPA_COVER, 0);
    lv_obj_clear_flag(parent, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(parent, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(parent, touch_event, LV_EVENT_PRESSED, nullptr);

    s_left_eye = create_eye(parent);
    s_right_eye = create_eye(parent);
    lv_obj_clear_flag(s_left_eye, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_clear_flag(s_right_eye, LV_OBJ_FLAG_CLICKABLE);
    set_geometry(s_left_eye, SCREEN_W / 2 - EYE_OFFSET_X - EYE_W / 2, SCREEN_H / 2 - 2, EYE_W, 4);
    set_geometry(s_right_eye, SCREEN_W / 2 + EYE_OFFSET_X - EYE_W / 2, SCREEN_H / 2 - 2, EYE_W, 4);

    s_last_frame_us = esp_timer_get_time();
    lv_timer_create(animate, 8, nullptr);
}

void robot_eyes_on_shake() {
    s_shake_pending.store(true);
}
