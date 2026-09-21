#include "robot_eyes.h"

#include <algorithm>
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
constexpr int THINKING_DOT_COUNT = 3;
constexpr int FRAME_MS = 33;
constexpr float IDLE_BEFORE_SLEEP_S = 22.0f;
constexpr float SLEEP_DURATION_S = 22.0f;
constexpr float SLEEP_TRANSITION_S = 0.9f;
constexpr float WAKE_TRANSITION_S = 0.55f;
constexpr float DIZZY_DURATION_S = 2.8f;
constexpr float CHARGE_DURATION_S = 2.7f;
constexpr int ANGRY_STRIPES = 9;
constexpr uint32_t CYAN = 0x4FE3FF;
constexpr uint32_t GREEN = 0x42F58D;
constexpr uint32_t RED = 0xFF4057;
constexpr uint32_t PURPLE = 0xB58CFF;
constexpr uint32_t DIM = 0x3F5260;

static lv_obj_t *s_left_eye;
static lv_obj_t *s_right_eye;
static lv_obj_t *s_mouth;
static lv_obj_t *s_thinking_dots[THINKING_DOT_COUNT];
static lv_obj_t *s_sleep_z[3];
static lv_obj_t *s_angry_left;
static lv_obj_t *s_angry_right;
static lv_obj_t *s_angry_left_stripes[ANGRY_STRIPES];
static lv_obj_t *s_angry_right_stripes[ANGRY_STRIPES];
static lv_obj_t *s_charge_bolt;
static lv_timer_t *s_timer;
static bool s_active;
static bool s_angry;
static bool s_angry_visible;
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
enum RemoteCommand {
    REMOTE_NONE = 0,
    REMOTE_IDLE,
    REMOTE_BLINK,
    REMOTE_SLEEP,
    REMOTE_ANGRY,
    REMOTE_DIZZY,
};
enum AssistantState {
    ASSISTANT_IDLE = 0,
    ASSISTANT_LISTENING,
    ASSISTANT_THINKING,
    ASSISTANT_SPEAKING,
    ASSISTANT_ERROR,
    ASSISTANT_OFFLINE,
};

static std::atomic<bool> s_shake_pending{false};
static std::atomic<bool> s_charge_pending{false};
static std::atomic<int> s_remote_command{REMOTE_NONE};
static std::atomic<int> s_assistant_state{ASSISTANT_IDLE};

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
        s_expression = 0;
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

static void show_angry_eyes(bool show) {
    if (show == s_angry_visible) return;
    s_angry_visible = show;
    if (show) {
        lv_obj_add_flag(s_left_eye, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(s_right_eye, LV_OBJ_FLAG_HIDDEN);
        lv_obj_clear_flag(s_angry_left, LV_OBJ_FLAG_HIDDEN);
        lv_obj_clear_flag(s_angry_right, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_clear_flag(s_left_eye, LV_OBJ_FLAG_HIDDEN);
        lv_obj_clear_flag(s_right_eye, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(s_angry_left, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(s_angry_right, LV_OBJ_FLAG_HIDDEN);
    }
}

static void set_angry_geometry(lv_obj_t *container, lv_obj_t **stripes, bool mirror,
                               int center_x, int center_y, int height) {
    height = height < 4 ? 4 : height;
    const int y = center_y - height / 2;
    set_geometry(container, center_x - EYE_W / 2, y, EYE_W, height);
    for (int i = 0; i < ANGRY_STRIPES; ++i) {
        const int top = i * height / ANGRY_STRIPES;
        const int bottom = (i + 1) * height / ANGRY_STRIPES;
        const int stripe_h = std::max(1, bottom - top + 1);
        const int width = std::max(4, (i + 1) * EYE_W / ANGRY_STRIPES);
        const int x = mirror ? EYE_W - width : 0;
        set_geometry(stripes[i], x, top, width, stripe_h);
    }
}

static void update_charge_bolt() {
    if (s_time >= s_charge_until || s_angry) {
        lv_obj_set_style_opa(s_charge_bolt, LV_OPA_TRANSP, 0);
        return;
    }
    const float age = CHARGE_DURATION_S - (s_charge_until - s_time);
    const float fade = age < 0.18f ? age / 0.18f : std::max(0.0f, 1.0f - (age - 0.18f) / 0.72f);
    lv_obj_set_pos(s_charge_bolt, 210, 35 - static_cast<int>(age * 8.0f));
    lv_obj_set_style_opa(s_charge_bolt, static_cast<lv_opa_t>(fade * 255.0f), 0);
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

static void update_thinking_dots(int assistant_state) {
        const bool visible = assistant_state == ASSISTANT_THINKING && s_sleep_started < 0.0f;
    for (int i = 0; i < THINKING_DOT_COUNT; ++i) {
        if (!visible) {
            lv_obj_set_style_opa(s_thinking_dots[i], LV_OPA_TRANSP, 0);
            continue;
        }
        // Each dot grows and shrinks in sequence, with a tiny vertical hover.
        const float phase = std::fmod(s_time * 2.4f - static_cast<float>(i) * 0.72f + 9.0f, 3.0f);
        const float distance = std::fabs(phase - 1.5f) / 1.5f;
        const float pulse = 1.0f - std::min(1.0f, distance);
        const int diameter = 13 + static_cast<int>(pulse * 11.0f);
        const int x = SCREEN_W / 2 - 42 + i * 42 - diameter / 2;
        const int y = 72 - static_cast<int>(pulse * 4.0f) - diameter / 2;
        lv_obj_set_size(s_thinking_dots[i], diameter, diameter);
        lv_obj_set_pos(s_thinking_dots[i], x, y);
        lv_obj_set_style_bg_color(s_thinking_dots[i],
                                  lv_color_hex(assistant_state == ASSISTANT_THINKING ? RED : PURPLE), 0);
        lv_obj_set_style_opa(s_thinking_dots[i], LV_OPA_COVER, 0);
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

    const int assistant_state = s_assistant_state.load();
    const int remote = s_remote_command.exchange(REMOTE_NONE);
    if (remote != REMOTE_NONE) {
        if (remote == REMOTE_IDLE) {
            s_angry = false;
            s_dizzy_until = s_time;
            s_charge_until = s_time;
            wake_up(false);
        } else if (remote == REMOTE_BLINK) {
            s_angry = false;
            wake_up(false);
            s_blink_time = 0.0f;
            s_blinks_left = 1;
        } else if (remote == REMOTE_SLEEP && assistant_state == ASSISTANT_IDLE) {
            s_angry = false;
            s_dizzy_until = s_time;
            s_charge_until = s_time;
            s_sleep_started = s_time;
            s_target_x = 0.0f;
            s_target_y = 0.0f;
        } else if (remote == REMOTE_ANGRY) {
            s_angry = true;
            s_tap_count = 0;
            s_blinks_left = 0;
            s_blink_time = -1.0f;
            s_expression = 0;
            s_sleep_started = -1.0f;
            s_dizzy_until = s_time;
            s_charge_until = s_time;
        } else if (remote == REMOTE_DIZZY) {
            s_angry = false;
            wake_up(false);
            s_dizzy_until = s_time + DIZZY_DURATION_S;
        }
    }

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

    if (s_sleep_started >= 0.0f && assistant_state != ASSISTANT_IDLE) wake_up(false);

    if (!s_angry && assistant_state == ASSISTANT_IDLE && s_sleep_started < 0.0f && s_time >= s_dizzy_until &&
        s_time >= s_charge_until && s_time - s_last_activity >= IDLE_BEFORE_SLEEP_S) {
        s_sleep_started = s_time;
        s_target_x = 0.0f;
        s_target_y = 0.0f;
    }
    if (s_sleep_started >= 0.0f && s_time - s_sleep_started >= SLEEP_DURATION_S) wake_up(false);

    if (s_sleep_started < 0.0f && s_time >= s_dizzy_until) {
        s_next_gaze -= dt;
        if (s_next_gaze <= 0.0f) {
            s_next_gaze = 0.8f + random_unit() * 2.0f;
            s_target_x = (random_unit() * 2.0f - 1.0f) * 17.0f;
            s_target_y = (random_unit() * 2.0f - 1.0f) * 10.0f;
        }
        if (!s_angry && s_time >= s_next_expression && s_time >= s_expression_until) {
            s_expression = random_unit() < 0.55f ? 1 : 2;
            s_expression_until = s_time + 1.0f + random_unit() * 1.1f;
            s_next_expression = s_expression_until + 4.0f + random_unit() * 4.0f;
        }
        if (!s_angry && s_time >= s_expression_until) s_expression = 0;
    } else {
        s_expression = 0;
    }

    const float follow = 1.0f - expf(-dt * 8.0f);
    s_gaze_x = approach(s_gaze_x, s_target_x, follow);
    s_gaze_y = approach(s_gaze_y, s_target_y, follow);

    float closure = 0.0f;
    if (s_blinks_left > 0 && s_blink_time < 0.0f) {
        s_blink_time += dt;
    } else if (s_blink_time >= 0.0f) {
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
    } else if (s_blinks_left == 0) {
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
    if (assistant_state == ASSISTANT_THINKING) {
        left_height = std::max(30, static_cast<int>(left_height * 0.62f));
        right_height = std::max(30, static_cast<int>(right_height * 0.62f));
    }
    if (s_angry) {
        // Compact, inward-slanting wedges like the reference face.
        left_height = 46;
        right_height = 46;
    }

    if (s_time < s_dizzy_until) {
        const float phase = (DIZZY_DURATION_S - (s_dizzy_until - s_time)) * 11.0f;
        gaze_x = static_cast<int>(sinf(phase) * 18.0f);
        gaze_y = static_cast<int>(cosf(phase * 0.83f) * 12.0f);
        left_height = static_cast<int>(EYE_H * (0.72f + 0.18f * sinf(phase * 1.31f)));
        right_height = static_cast<int>(EYE_H * (0.72f - 0.18f * sinf(phase * 1.31f)));
    }

    /* LVGL coordinates are integer pixels. The former 5 px/s peak speed spent
       several frames on the same row, so the gentle bob looked stepped. This
       reaches about 25 px/s without increasing redraw frequency. */
    float bob = sinf(s_time * 2.20f) * 11.0f + sinf(s_time * 0.65f) * 2.0f;
    if (s_angry) {
        set_face_color(RED);
    } else if (assistant_state == ASSISTANT_LISTENING) {
        set_face_color(GREEN);
    } else if (assistant_state == ASSISTANT_THINKING) {
        set_face_color(PURPLE);
    } else if (assistant_state == ASSISTANT_ERROR) {
        set_face_color(RED);
    } else if (assistant_state == ASSISTANT_OFFLINE) {
        set_face_color(DIM);
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
    show_angry_eyes(s_angry);
    set_geometry(s_left_eye, left_center_x - left_width / 2, center_y - left_height / 2,
                 left_width, left_height);
    set_geometry(s_right_eye, right_center_x - right_width / 2, center_y - right_height / 2,
                 right_width, right_height);
    if (s_angry) {
        set_angry_geometry(s_angry_left, s_angry_left_stripes, false,
                           left_center_x, center_y, left_height);
        set_angry_geometry(s_angry_right, s_angry_right_stripes, true,
                           right_center_x, center_y, right_height);
        lv_obj_set_style_transform_rotation(s_angry_left, -140, 0);
        lv_obj_set_style_transform_rotation(s_angry_right, 140, 0);
    } else {
        lv_obj_set_style_transform_rotation(s_angry_left, 0, 0);
        lv_obj_set_style_transform_rotation(s_angry_right, 0, 0);
    }
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
    if (assistant_state == ASSISTANT_SPEAKING) {
        const float voice_pulse = 0.35f + 0.65f * fabsf(sinf(s_time * 15.0f));
        set_geometry(s_mouth, mouth_x, mouth_y - static_cast<int>(voice_pulse * 11.0f),
                     mouth_w, MOUTH_H + static_cast<int>(voice_pulse * 22.0f));
    } else {
        set_geometry(s_mouth, mouth_x, mouth_y, mouth_w, MOUTH_H);
    }
    lv_obj_set_style_transform_rotation(s_mouth, mouth_rotation, 0);
    update_sleep_marks(static_cast<float>(center_y));
    update_thinking_dots(assistant_state);
    update_charge_bolt();
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

static lv_obj_t *create_angry_eye(lv_obj_t *parent, lv_obj_t **stripes) {
    lv_obj_t *container = lv_obj_create(parent);
    lv_obj_remove_style_all(container);
    lv_obj_set_size(container, EYE_W, EYE_H);
    lv_obj_clear_flag(container, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_clear_flag(container, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(container, LV_OBJ_FLAG_HIDDEN);
    for (int i = 0; i < ANGRY_STRIPES; ++i) {
        stripes[i] = lv_obj_create(container);
        lv_obj_remove_style_all(stripes[i]);
        lv_obj_set_style_bg_color(stripes[i], lv_color_hex(RED), 0);
        lv_obj_set_style_bg_opa(stripes[i], LV_OPA_COVER, 0);
        lv_obj_set_style_radius(stripes[i], 3, 0);
        lv_obj_clear_flag(stripes[i], LV_OBJ_FLAG_CLICKABLE);
        lv_obj_clear_flag(stripes[i], LV_OBJ_FLAG_SCROLLABLE);
    }
    return container;
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
    for (int i = 0; i < THINKING_DOT_COUNT; ++i) {
        s_thinking_dots[i] = lv_obj_create(parent);
        lv_obj_remove_style_all(s_thinking_dots[i]);
        lv_obj_set_style_bg_color(s_thinking_dots[i], lv_color_hex(PURPLE), 0);
        lv_obj_set_style_bg_opa(s_thinking_dots[i], LV_OPA_COVER, 0);
        lv_obj_set_style_radius(s_thinking_dots[i], LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_opa(s_thinking_dots[i], LV_OPA_TRANSP, 0);
        lv_obj_set_size(s_thinking_dots[i], 18, 18);
        lv_obj_set_pos(s_thinking_dots[i], SCREEN_W / 2 - 51 + i * 42, 63);
        lv_obj_clear_flag(s_thinking_dots[i], LV_OBJ_FLAG_CLICKABLE);
    }
    s_angry_left = create_angry_eye(parent, s_angry_left_stripes);
    s_angry_right = create_angry_eye(parent, s_angry_right_stripes);
    lv_obj_clear_flag(s_left_eye, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_clear_flag(s_right_eye, LV_OBJ_FLAG_CLICKABLE);
    set_geometry(s_left_eye, SCREEN_W / 2 - EYE_OFFSET_X - EYE_W / 2, SCREEN_H / 2 - 2, EYE_W, 4);
    set_geometry(s_right_eye, SCREEN_W / 2 + EYE_OFFSET_X - EYE_W / 2, SCREEN_H / 2 - 2, EYE_W, 4);
    set_geometry(s_mouth, SCREEN_W / 2 - MOUTH_W / 2, MOUTH_Y, MOUTH_W, MOUTH_H);

    s_charge_bolt = lv_label_create(parent);
    lv_label_set_text(s_charge_bolt, LV_SYMBOL_CHARGE);
    lv_obj_set_style_text_font(s_charge_bolt, &lv_font_montserrat_24, 0);
    lv_obj_set_style_text_color(s_charge_bolt, lv_color_hex(0xFFE45C), 0);
    lv_obj_set_style_opa(s_charge_bolt, LV_OPA_TRANSP, 0);
    lv_obj_clear_flag(s_charge_bolt, LV_OBJ_FLAG_CLICKABLE);

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


void robot_eyes_remote_idle() {
    s_remote_command.store(REMOTE_IDLE);
}

void robot_eyes_remote_blink() {
    s_remote_command.store(REMOTE_BLINK);
}

void robot_eyes_remote_sleep() {
    s_remote_command.store(REMOTE_SLEEP);
}

void robot_eyes_remote_angry() {
    s_remote_command.store(REMOTE_ANGRY);
}

void robot_eyes_remote_dizzy() {
    s_remote_command.store(REMOTE_DIZZY);
}

void robot_eyes_assistant_idle() { s_assistant_state.store(ASSISTANT_IDLE); }
void robot_eyes_assistant_listening() { s_assistant_state.store(ASSISTANT_LISTENING); }
void robot_eyes_assistant_thinking() { s_assistant_state.store(ASSISTANT_THINKING); }
void robot_eyes_assistant_speaking() { s_assistant_state.store(ASSISTANT_SPEAKING); }
void robot_eyes_assistant_error() { s_assistant_state.store(ASSISTANT_ERROR); }
void robot_eyes_assistant_offline() { s_assistant_state.store(ASSISTANT_OFFLINE); }
