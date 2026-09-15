#include "robot_eyes.h"

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

static float random_unit() {
    return static_cast<float>(esp_random()) / static_cast<float>(UINT32_MAX);
}

static float approach(float current, float target, float amount) {
    return current + (target - current) * amount;
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

    s_next_gaze -= dt;
    if (s_next_gaze <= 0.0f) {
        s_next_gaze = 0.9f + random_unit() * 2.3f;
        s_target_x = (random_unit() * 2.0f - 1.0f) * 10.0f;
        s_target_y = (random_unit() * 2.0f - 1.0f) * 6.0f;
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

    float startup = s_time / 0.55f;
    if (startup > 1.0f) startup = 1.0f;
    startup = startup * startup * (3.0f - 2.0f * startup);
    const int height = static_cast<int>(4.0f + (EYE_H - 4.0f) * startup * (1.0f - closure * 0.96f));
    const int center_y = SCREEN_H / 2 + static_cast<int>(sinf(s_time * 1.1f) * 2.0f + s_gaze_y);
    const int left_center_x = SCREEN_W / 2 - EYE_OFFSET_X + static_cast<int>(s_gaze_x);
    const int right_center_x = SCREEN_W / 2 + EYE_OFFSET_X + static_cast<int>(s_gaze_x);
    const int top = center_y - height / 2;

    set_geometry(s_left_eye, left_center_x - EYE_W / 2, top, EYE_W, height);
    set_geometry(s_right_eye, right_center_x - EYE_W / 2, top, EYE_W, height);
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

    s_left_eye = create_eye(parent);
    s_right_eye = create_eye(parent);
    set_geometry(s_left_eye, SCREEN_W / 2 - EYE_OFFSET_X - EYE_W / 2, SCREEN_H / 2 - 2, EYE_W, 4);
    set_geometry(s_right_eye, SCREEN_W / 2 + EYE_OFFSET_X - EYE_W / 2, SCREEN_H / 2 - 2, EYE_W, 4);

    s_last_frame_us = esp_timer_get_time();
    lv_timer_create(animate, 8, nullptr);
}
