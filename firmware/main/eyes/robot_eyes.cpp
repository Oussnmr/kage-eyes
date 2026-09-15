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
constexpr int MOUTH_W = 46;
constexpr int MOUTH_H = 7;
constexpr int MOUTH_Y = 258;
constexpr int FRAME_MS = 33;
constexpr float IDLE_BEFORE_SLEEP_S = 22.0f;
constexpr float SLEEP_DURATION_S = 22.0f;
constexpr float SLEEP_TRANSITION_S = 0.9f;
constexpr float WAKE_TRANSITION_S = 0.55f;
constexpr float DIZZY_DURATION_S = 2.8f;
constexpr float CHARGE_DURATION_S = 2.7f;
constexpr uint32_t CYAN = 0x4FE3FF;
constexpr uint32_t GREEN = 0x42F58D;
constexpr uint32_t RED = 0xFF4057;

static lv_obj_t *s_left_eye;
static lv_obj_t *s_right_eye;
static lv_obj_t *s_mouth;
static lv_obj_t *s_sleep_z[3];
static lv_timer_t *s_timer;
static bool s_active;
static bool s_angry;
static int s_tap_count;
static int64_t s_last_tap_us;
static int64_t s_last_frame_us;
static float s_time;
static float s_next_blink = 2.0f;
static float s_blink_time = -1.0f;
static int s_blinks_left;
static float s_next_gaze = 1.0f;
static float s_gaze_x;
static float s_gaze_y;
static float s_target_x;
static float s_target_y;
static float s_last_activity;
static float s_sleep_started = -1.0f;
static float s_wake_started;
static float s_dizzy_until;
static float s_charge_until;
static float s_next_expression = 4.0f;
static float s_expression_until;
static int s_expression;
static uint32_t s_face_color;
static std::atomic<bool> s_shake_pending{false};
static std::atomic<bool> s_charge_pending{false};

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

static void set_face_color(uint32_t color) {
    if (color == s_face_color) return;
    s_face_color = color;
    lv_obj_set_style_bg_color(s_left_eye, lv_color_hex(color), 0);
    lv_obj_set_style_bg_color(s_right_eye, lv_color_hex(color), 0);
    lv_obj_set_style_bg_color(s_mouth, lv_color_hex(color), 0);
}

static void wake_up(bool blink_if_awake) {
    const bool was_sleeping = s_sleep_started >= 0.0f;
    s_sleep_started = -1.0f;
    s_last_activity = s_time;
    s_wake_started = s_time;
    s_target_x = 0.0f;
    s_target_y = 0.0f;
    if (!was_sleeping && blink_if_awake) {
        s_blink_time = 0.0f;
        s_blinks_left = 1;
    }
}

static void touch_event(lv_event_t *) {
    const int64_t now = esp_timer_get_time();
    if (s_angry) {
        s_angry = false;
        s_tap_count = 0;
        wake_up(false);
        return;
    }
    if (now - s_last_tap_us > 520000) s_tap_count = 0;
    s_last_tap_us = now;
    ++s_tap_count;
    wake_up(true);
    if (s_tap_count >= 5) {
        s_angry = true;
        s_tap_count = 0;
        s_blinks_left = 0;
        s_blink_time = -1.0f;
        s_dizzy_until = s_time;
        s_charge_until = s_time;
    }
}

static void set_geometry(lv_obj_t *eye, int x, int y, int width, int height) {
    if (lv_obj_get_x(eye) != x || lv_obj_get_y(eye) != y) lv_obj_set_pos(eye, x, y);
    if (lv_obj_get_width(eye) != width || lv_obj_get_height(eye) != height) {
        lv_obj_set_size(eye, width, height);
    }
}

static void update_sleep_marks(float center_y) {
    if (s_sleep_started < 0.0f || s_angry) {
        for (lv_obj_t *mark : s_sleep_z) lv_obj_set_style_opa(mark, LV_OPA_TRANSP, 0);
        return;
    }
    const float elapsed = s_time - s_sleep_started;
    for (int i = 0; i < 3; ++i) {
        const float progress = std::fmod(elapsed + i * 0.78f, 2.34f) / 2.34f;
        const int x = 310 + static_cast<int>(progress * 34.0f) + i * 4;
        const int y = static_cast<int>(center_y - 25.0f - progress * 92.0f);
        const float fade = std::sin(progress * 3.14159265f);
        lv_obj_set_pos(s_sleep_z[i], x, y);
        lv_obj_set_style_opa(s_sleep_z[i], static_cast<lv_opa_t>(fade * 230.0f), 0);
    }
}

static void animate(lv_timer_t *) {
    if (!s_active) return;
    const int64_t now = esp_timer_get_time();
    if (now - s_last_frame_us < FRAME_MS * 1000) return;

    float dt = static_cast<float>(now - s_last_frame_us) / 1000000.0f;
    s_last_frame_us = now;
    if (dt > 0.08f) dt = 0.08f;
    s_time += dt;

    if (s_shake_pending.exchange(false)) {
        if (s_angry) {
            s_angry = false;
            wake_up(false);
        } else {
            wake_up(false);
            s_dizzy_until = s_time + DIZZY_DURATION_S;
        }
    }
    if (s_charge_pending.exchange(false)) {
        wake_up(false);
        if (!s_angry) s_charge_until = s_time + CHARGE_DURATION_S;
    }

    if (!s_angry && s_sleep_started < 0.0f && s_time >= s_dizzy_until &&
        s_time >= s_charge_until && s_time - s_last_activity >= IDLE_BEFORE_SLEEP_S) {
        s_sleep_started = s_time;
        s_target_x = 0.0f;
        s_target_y = 0.0f;
    }
    if (s_sleep_started >= 0.0f && s_time - s_sleep_started >= SLEEP_DURATION_S) wake_up(false);

    if (!s_angry && s_sleep_started < 0.0f && s_time >= s_dizzy_until) {
        s_next_gaze -= dt;
        if (s_next_gaze <= 0.0f) {
            s_next_gaze = 0.8f + random_unit() * 2.0f;
            s_target_x = (random_unit() * 2.0f - 1.0f) * 17.0f;
            s_target_y = (random_unit() * 2.0f - 1.0f) * 10.0f;
        }
        if (s_time >= s_next_expression && s_time >= s_expression_until) {
            s_expression = random_unit() < 0.55f ? 1 : 2;
            s_expression_until = s_time + 1.0f + random_unit() * 1.1f;
            s_next_expression = s_expression_until + 4.0f + random_unit() * 4.0f;
        }
        if (s_time >= s_expression_until) s_expression = 0;
    } else {
        s_expression = 0;
    }

    const float follow = 1.0f - expf(-dt * 8.0f);
    s_gaze_x = approach(s_gaze_x, s_target_x, follow);
    s_gaze_y = approach(s_gaze_y, s_target_y, follow);

    float closure = 0.0f;
    if (!s_angry && s_blinks_left > 0 && s_blink_time < 0.0f) {
        s_blink_time += dt;
    } else if (!s_angry && s_blink_time >= 0.0f) {
        s_blink_time += dt;
        constexpr float CLOSE_S = 0.07f;
        constexpr float OPEN_S = 0.16f;
        if (s_blink_time >= CLOSE_S + OPEN_S) {
            if (--s_blinks_left > 0) s_blink_time = -0.09f;
            else {
                s_blink_time = -1.0f;
                s_next_blink = 3.0f + random_unit() * 2.7f;
            }
        } else {
            closure = s_blink_time < CLOSE_S
                ? smoothstep(s_blink_time / CLOSE_S)
                : 1.0f - smoothstep((s_blink_time - CLOSE_S) / OPEN_S);
        }
    } else if (!s_angry && s_blinks_left == 0) {
        s_next_blink -= dt;
        if (s_next_blink <= 0.0f) {
            s_blink_time = 0.0f;
            s_blinks_left = random_unit() < 0.25f ? 2 : 1;
        }
    }

    float open = smoothstep((s_time - s_wake_started) / WAKE_TRANSITION_S);
    if (s_sleep_started >= 0.0f) {
        const float closing = smoothstep((s_time - s_sleep_started) / SLEEP_TRANSITION_S);
        open *= 1.0f - closing * 0.94f;
        closure = 0.0f;
    }

    int gaze_x = static_cast<int>(s_gaze_x);
    int gaze_y = static_cast<int>(s_gaze_y);
    int left_width = EYE_W;
    int right_width = EYE_W;
    int left_height = static_cast<int>(4.0f + (EYE_H - 4.0f) * open * (1.0f - closure * 0.96f));
    int right_height = left_height;
    if (s_expression == 1) {
        left_height = static_cast<int>(left_height * 0.76f);
        right_height = static_cast<int>(right_height * 1.06f);
    } else if (s_expression == 2) {
        left_height = static_cast<int>(left_height * 0.68f);
        right_height = left_height;
        left_width += 8;
        right_width += 8;
    }

    if (s_time < s_dizzy_until) {
        const float phase = (DIZZY_DURATION_S - (s_dizzy_until - s_time)) * 11.0f;
        gaze_x = static_cast<int>(sinf(phase) * 18.0f);
        gaze_y = static_cast<int>(cosf(phase * 0.83f) * 12.0f);
        left_height = static_cast<int>(EYE_H * (0.72f + 0.18f * sinf(phase * 1.31f)));
        right_height = static_cast<int>(EYE_H * (0.72f - 0.18f * sinf(phase * 1.31f)));
    }

    float bob = sinf(s_time * 1.12f) * 4.0f + sinf(s_time * 0.47f) * 1.5f;
    if (s_angry) {
        gaze_x = 0;
        gaze_y = 4;
        left_height = 78;
        right_height = 78;
        bob = 0.0f;
        set_face_color(RED);
    } else if (s_time < s_charge_until) {
        const float remaining = s_charge_until - s_time;
        const float pulse = 0.5f + 0.5f * sinf((CHARGE_DURATION_S - remaining) * 8.0f);
        left_height = static_cast<int>(left_height * (0.92f + pulse * 0.12f));
        right_height = left_height;
        set_face_color(GREEN);
    } else {
        set_face_color(CYAN);
    }

    const int center_y = SCREEN_H / 2 + static_cast<int>(bob) + gaze_y;
    const int left_center_x = SCREEN_W / 2 - EYE_OFFSET_X + gaze_x;
    const int right_center_x = SCREEN_W / 2 + EYE_OFFSET_X + gaze_x;
    set_geometry(s_left_eye, left_center_x - left_width / 2, center_y - left_height / 2,
                 left_width, left_height);
    set_geometry(s_right_eye, right_center_x - right_width / 2, center_y - right_height / 2,
                 right_width, right_height);
    int mouth_w = MOUTH_W + static_cast<int>(bob * 0.5f);
    int mouth_y = MOUTH_Y + static_cast<int>(bob);
    int mouth_x = SCREEN_W / 2 - mouth_w / 2;
    int mouth_rotation = 0;
    if (s_sleep_started >= 0.0f) mouth_w = 34;
    if (s_expression == 1) mouth_rotation = -45;
    if (s_expression == 2) mouth_w = 62;
    if (s_angry) mouth_w = 54;
    if (s_time < s_dizzy_until) {
        const float phase = (DIZZY_DURATION_S - (s_dizzy_until - s_time)) * 11.0f;
        mouth_w = 52;
        mouth_x = SCREEN_W / 2 - mouth_w / 2 + static_cast<int>(sinf(phase * 0.7f) * 5.0f);
        mouth_y += static_cast<int>(cosf(phase * 0.8f) * 4.0f);
        mouth_rotation = static_cast<int>(sinf(phase * 0.55f) * 120.0f);
    }
    set_geometry(s_mouth, mouth_x, mouth_y, mouth_w, MOUTH_H);
    lv_obj_set_style_transform_rotation(s_mouth, mouth_rotation, 0);
    update_sleep_marks(static_cast<float>(center_y));
}

static lv_obj_t *create_eye(lv_obj_t *parent) {
    lv_obj_t *eye = lv_obj_create(parent);
    lv_obj_remove_style_all(eye);
    lv_obj_set_style_bg_opa(eye, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(eye, lv_color_hex(CYAN), 0);
    lv_obj_set_style_radius(eye, EYE_RADIUS, 0);
    lv_obj_clear_flag(eye, LV_OBJ_FLAG_SCROLLABLE);
    return eye;
}

static lv_obj_t *create_mouth(lv_obj_t *parent) {
    lv_obj_t *mouth = lv_obj_create(parent);
    lv_obj_remove_style_all(mouth);
    lv_obj_set_style_bg_opa(mouth, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(mouth, lv_color_hex(CYAN), 0);
    lv_obj_set_style_radius(mouth, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_transform_pivot_x(mouth, MOUTH_W / 2, 0);
    lv_obj_set_style_transform_pivot_y(mouth, MOUTH_H / 2, 0);
    lv_obj_clear_flag(mouth, LV_OBJ_FLAG_SCROLLABLE);
    return mouth;
}
}  // namespace

void robot_eyes_begin(lv_obj_t *parent) {
    lv_obj_set_style_bg_color(parent, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(parent, LV_OPA_COVER, 0);
    lv_obj_clear_flag(parent, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(parent, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(parent, touch_event, LV_EVENT_SHORT_CLICKED, nullptr);

    s_left_eye = create_eye(parent);
    s_right_eye = create_eye(parent);
    s_mouth = create_mouth(parent);
    lv_obj_clear_flag(s_left_eye, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_clear_flag(s_right_eye, LV_OBJ_FLAG_CLICKABLE);
    set_geometry(s_left_eye, SCREEN_W / 2 - EYE_OFFSET_X - EYE_W / 2, SCREEN_H / 2 - 2, EYE_W, 4);
    set_geometry(s_right_eye, SCREEN_W / 2 + EYE_OFFSET_X - EYE_W / 2, SCREEN_H / 2 - 2, EYE_W, 4);
    set_geometry(s_mouth, SCREEN_W / 2 - MOUTH_W / 2, MOUTH_Y, MOUTH_W, MOUTH_H);

    for (int i = 0; i < 3; ++i) {
        s_sleep_z[i] = lv_label_create(parent);
        lv_label_set_text(s_sleep_z[i], "Z");
        lv_obj_set_style_text_font(s_sleep_z[i], i == 2 ? &lv_font_montserrat_24 : &lv_font_montserrat_20, 0);
        lv_obj_set_style_text_color(s_sleep_z[i], lv_color_hex(CYAN), 0);
        lv_obj_set_style_opa(s_sleep_z[i], LV_OPA_TRANSP, 0);
        lv_obj_clear_flag(s_sleep_z[i], LV_OBJ_FLAG_CLICKABLE);
    }

    s_face_color = CYAN;
    s_last_frame_us = esp_timer_get_time();
    s_active = true;
    s_timer = lv_timer_create(animate, 8, nullptr);
}

void robot_eyes_set_active(bool active) {
    if (s_active == active) return;
    s_active = active;
    if (active) {
        s_last_frame_us = esp_timer_get_time();
        wake_up(false);
        if (s_timer) lv_timer_resume(s_timer);
    } else if (s_timer) {
        lv_timer_pause(s_timer);
    }
}

void robot_eyes_on_shake() {
    s_shake_pending.store(true);
}

void robot_eyes_on_charge_started() {
    s_charge_pending.store(true);
}
