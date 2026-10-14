#include "robot_eyes.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstdlib>

#include "esp_random.h"
#include "esp_timer.h"
#include "services/kage_bridge.h"
#include "services/robot_motion.h"

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
constexpr int SPEED_LINE_COUNT = 4;
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
constexpr uint32_t PURPLE = 0xB58CFF;
constexpr uint32_t DIM = 0x3F5260;
constexpr uint32_t ORANGE = 0xFF9D3D;
constexpr uint32_t WHITE = 0xF4FAFF;
constexpr uint32_t YELLOW = 0xFFD84A;

static lv_obj_t *s_left_eye;
static lv_obj_t *s_right_eye;
static lv_obj_t *s_custom_eye_layer;
static std::atomic<int> s_custom_face[16];
static std::atomic<bool> s_custom_face_active{false};
static int s_custom_left_x = 112, s_custom_right_x = 336, s_custom_center_y = 184;
static int s_custom_left_w = EYE_W, s_custom_right_w = EYE_W;
static int s_custom_left_h = EYE_H, s_custom_right_h = EYE_H;
static lv_obj_t *s_mouth;
static lv_obj_t *s_smile;
static lv_obj_t *s_angry_left_brow;
static lv_obj_t *s_angry_right_brow;
static lv_obj_t *s_thinking_dots[THINKING_DOT_COUNT];
static lv_obj_t *s_speed_lines[SPEED_LINE_COUNT];
static lv_obj_t *s_sleep_z[3];
static lv_obj_t *s_charge_bolt;
static lv_obj_t *s_voice_indicator;
static lv_obj_t *s_typing_preview;
static lv_timer_t *s_timer;
static bool s_active;
static bool s_angry;
static std::atomic<bool> s_voice_active{false};
static int s_tap_count;
static bool s_hold_touch_active;
static bool s_second_press_candidate;
static int64_t s_last_tap_us;
static int64_t s_last_frame_us;
static float s_time;
// Natural idle blinking: keep it sparse on the small display so it does not
// look like the eyes are constantly flickering.
static float s_next_blink = 6.0f;
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
static std::atomic<bool> s_activity_pending{false};
static std::atomic<bool> s_cancel_dizzy_pending{false};
static std::atomic<int> s_remote_command{REMOTE_NONE};
static std::atomic<int> s_assistant_state{ASSISTANT_IDLE};
static std::atomic<int> s_mouth_level{0};
static std::atomic<int> s_behavior{ROBOT_BEHAVIOR_NEUTRAL};
static std::atomic<int64_t> s_behavior_until_us{0};
static std::atomic<int> s_drive_forward{0};
static std::atomic<int> s_drive_turn{0};
static std::atomic<int64_t> s_drive_updated_us{0};
static std::atomic<int> s_manual_look_x{0};
static std::atomic<int> s_manual_look_y{0};
static std::atomic<int64_t> s_manual_look_updated_us{0};
static std::atomic<int64_t> s_typing_preview_until_us{0};
static lv_timer_t *s_tap_timer;

static lv_obj_t *preview_rect(lv_obj_t *parent, int x, int y, int width, int height,
                              uint32_t color, int radius = 0) {
    lv_obj_t *item = lv_obj_create(parent);
    lv_obj_remove_style_all(item);
    lv_obj_set_pos(item, x, y);
    lv_obj_set_size(item, width, height);
    lv_obj_set_style_bg_color(item, lv_color_hex(color), 0);
    lv_obj_set_style_bg_opa(item, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(item, radius, 0);
    lv_obj_clear_flag(item, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_clear_flag(item, LV_OBJ_FLAG_SCROLLABLE);
    return item;
}

// The Waveshare renderer clips transformed LVGL objects before rotating them.
// Draw the arm straight into the layer, just like simulator/typing_scene.cpp,
// so the physical display and simulator share the same uncut silhouette.
static void preview_line(lv_layer_t *layer, int x1, int y1, int x2, int y2,
                         int width, uint32_t color) {
    lv_draw_line_dsc_t line;
    lv_draw_line_dsc_init(&line);
    line.color = lv_color_hex(color);
    line.opa = LV_OPA_COVER;
    line.width = width;
    line.round_start = 1;
    line.round_end = 1;
    line.p1.x = x1;
    line.p1.y = y1;
    line.p2.x = x2;
    line.p2.y = y2;
    lv_draw_line(layer, &line);
}

static void draw_typing_preview_arm(lv_event_t *event) {
    lv_layer_t *layer = lv_event_get_layer(event);
    if (!layer) return;

    constexpr int sx = 92, sy = 251, tx = 69, ty = 186, thick = 29;
    constexpr int wrist_x = tx + thick / 5;
    constexpr int wrist_y = ty + thick / 2;
    // Black first creates the same clean negative-space outline used by the
    // simulator, then cyan fills the arm and its three rounded fingers.
    for (int pass = 0; pass < 2; ++pass) {
        const uint32_t color = pass == 0 ? 0x000000 : CYAN;
        const int border = pass == 0 ? 7 : 0;
        preview_line(layer, sx, sy, wrist_x, wrist_y, thick + border, color);
        for (int finger = -1; finger <= 1; ++finger) {
            constexpr int spread = thick / 2 + 2;
            const int fx = tx + finger * spread;
            const int fy = ty - (finger == 0 ? 9 : 2);
            preview_line(layer, wrist_x, wrist_y, fx, fy, thick * 2 / 3 + border, color);
        }
    }
}

static void create_typing_preview(lv_obj_t *parent) {
    s_typing_preview = preview_rect(parent, 0, 0, SCREEN_W, SCREEN_H, 0x000000);
    lv_obj_set_style_opa(s_typing_preview, LV_OPA_TRANSP, 0);
    lv_obj_add_event_cb(s_typing_preview, draw_typing_preview_arm, LV_EVENT_DRAW_MAIN, nullptr);
    // Keep this overlay in lockstep with simulator/typing_scene.cpp defaults.
    preview_rect(s_typing_preview, 140, 87, 112, 112, CYAN, 28);
    preview_rect(s_typing_preview, 313, 87, 112, 112, CYAN, 28);
    preview_rect(s_typing_preview, 262, 212, 39, 6, CYAN, 3);

    constexpr int x = 10, y = 245, w = 428, h = 110;
    constexpr int pad = 7, inset = 12, rows = 4, gap = 3, key_gap = 4;
    constexpr int interior_w = w - 2 * inset, interior_h = h - 2 * inset;
    constexpr int key_h = (interior_h - (rows - 1) * gap) / rows;
    preview_rect(s_typing_preview, x, y, w, h, WHITE, 12);
    preview_rect(s_typing_preview, x + pad, y + pad, w - 2 * pad, h - 2 * pad, 0x000000, 8);
    for (int row = 0; row < rows; ++row) {
        const int yy = y + inset + row * (key_h + gap);
        const int cells = row == 0 ? 5 : 10;
        int x0 = x + inset;
        if (row == 0) {
            constexpr int proportions[] = {1, 1, 5, 1, 1};
            constexpr int available = interior_w - key_gap * 4;
            for (int i = 0; i < cells; ++i) {
                int kw = available * proportions[i] / 9;
                if (i == cells - 1) kw = x + inset + interior_w - x0;
                preview_rect(s_typing_preview, x0, yy, kw, key_h, WHITE, 6);
                x0 += kw + key_gap;
            }
        } else {
            const int available = interior_w - (cells - 1) * key_gap;
            for (int i = 0; i < cells; ++i) {
                const int end = x + inset + (available * (i + 1)) / cells + i * key_gap;
                preview_rect(s_typing_preview, x0, yy, end - x0, key_h, WHITE, 6);
                x0 = end + key_gap;
            }
        }
    }
    preview_rect(s_typing_preview, x + inset, y + inset + key_h + gap,
                 (interior_w - 9 * key_gap) / 10, key_h * 2 + gap, WHITE, 6);

}

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

static void set_geometry(lv_obj_t *eye, int x, int y, int width, int height);

static void draw_custom_eyes(lv_event_t *event) {
    if (!s_custom_face_active.load(std::memory_order_acquire)) return;
    int v[16];
    for (int i = 0; i < 16; ++i) v[i] = s_custom_face[i].load(std::memory_order_relaxed);
    lv_layer_t *layer = lv_event_get_layer(event);
    const lv_color_t color = lv_color_hex(static_cast<uint32_t>(v[0]));
    const bool rectangle = v[7] == v[13] && v[9] == v[11] &&
                           v[8] == v[10] && v[12] == v[14];
    const int centers[2] = {s_custom_left_x, s_custom_right_x};
    const int widths[2] = {s_custom_left_w, s_custom_right_w};
    const int heights[2] = {s_custom_left_h, s_custom_right_h};
    for (int eye = 0; eye < 2; ++eye) {
        const int center_x = centers[eye];
        const int cx = center_x + v[1], cy = s_custom_center_y + v[2];
        if (rectangle) {
            lv_area_t area = {cx + v[7] * widths[eye] / EYE_W,
                              cy + v[8] * heights[eye] / EYE_H,
                              cx + v[9] * widths[eye] / EYE_W - 1,
                              cy + v[12] * heights[eye] / EYE_H - 1};
            lv_draw_rect_dsc_t rect;
            lv_draw_rect_dsc_init(&rect);
            rect.bg_color = color;
            rect.bg_opa = LV_OPA_COVER;
            rect.radius = v[3] * std::min(widths[eye], heights[eye]) / EYE_W;
            lv_draw_rect(layer, &rect, &area);
        } else {
            for (int half = 0; half < 2; ++half) {
                lv_draw_triangle_dsc_t triangle;
                lv_draw_triangle_dsc_init(&triangle);
                triangle.color = color;
                triangle.opa = LV_OPA_COVER;
                const int order[2][4] = {{0, 1, 2, 3}, {0, 3, 2, 1}};
                const int index[3] = {order[eye][0], order[eye][half + 1], order[eye][half + 2]};
                for (int point = 0; point < 3; ++point) {
                    const int corner = index[point];
                    const int mirrored_x = eye == 0 ? v[7 + corner * 2] : -v[7 + corner * 2];
                    triangle.p[point].x = cx + mirrored_x * widths[eye] / EYE_W;
                    triangle.p[point].y = cy + v[8 + corner * 2] * heights[eye] / EYE_H;
                }
                lv_draw_triangle(layer, &triangle);
            }
        }
    }
}

static void set_face_color(uint32_t color) {
    if (color == s_face_color) return;
    s_face_color = color;
    lv_obj_set_style_bg_color(s_left_eye, lv_color_hex(color), 0);
    lv_obj_set_style_bg_color(s_right_eye, lv_color_hex(color), 0);
    lv_obj_set_style_bg_color(s_mouth, lv_color_hex(color), 0);
    lv_obj_set_style_arc_color(s_smile, lv_color_hex(color), LV_PART_INDICATOR);
}

static void update_speed_lines(bool visible, int speed, int center_y, uint32_t color) {
    if (!visible) {
        for (auto *line : s_speed_lines) {
            lv_obj_set_style_opa(line, LV_OPA_TRANSP, 0);
        }
        return;
    }
    const float travel = fmodf(s_time * (90.0f + speed * 1.1f), 34.0f);
    for (int i = 0; i < SPEED_LINE_COUNT; ++i) {
        const bool right = i >= 2;
        const int row = i % 2;
        const int width = 22 + row * 7;
        const int x = right ? 382 + static_cast<int>(travel)
                            : 66 - width - static_cast<int>(travel);
        const int y = center_y + (row == 0 ? -31 : 25);
        set_geometry(s_speed_lines[i], x, y, width, 8);
        lv_obj_set_style_bg_color(s_speed_lines[i], lv_color_hex(color), 0);
        const int opacity = 150 + static_cast<int>((34.0f - travel) * 3.0f);
        lv_obj_set_style_opa(s_speed_lines[i], static_cast<lv_opa_t>(std::min(255, opacity)), 0);
    }
}

static void wake_up(bool blink_if_awake) {
    const bool was_sleeping = s_sleep_started >= 0.0f;
    s_sleep_started = -1.0f;
    s_last_activity = s_time;
    // Mouth activity is reported many times per second during TTS. Restarting
    // the wake transition here made the eyes close/open with every update.
    if (was_sleeping) {
        s_wake_started = s_time;
        s_target_x = 0.0f;
        s_target_y = 0.0f;
    }
    if (!was_sleeping && blink_if_awake) {
        s_blink_time = 0.0f;
        s_blinks_left = 1;
    }
}

static void complete_tap_gesture(lv_timer_t *timer) {
    if (timer == s_tap_timer) s_tap_timer = nullptr;
    const int taps = s_tap_count;
    s_tap_count = 0;
    if (taps != 2) return;

    const auto state = s_assistant_state.load();
    if (state == ASSISTANT_IDLE) kage_bridge_wake_voice();
    else if (state == ASSISTANT_LISTENING) kage_bridge_sleep_voice();
    else if (state == ASSISTANT_THINKING || state == ASSISTANT_SPEAKING) kage_bridge_interrupt_voice();
}

static void cancel_tap_timer() {
    if (!s_tap_timer) return;
    lv_timer_delete(s_tap_timer);
    s_tap_timer = nullptr;
}

static void touch_event(lv_event_t *event) {
    const auto code = lv_event_get_code(event);
    if (code == LV_EVENT_PRESSED) {
        const int64_t now = esp_timer_get_time();
        s_second_press_candidate = s_tap_count == 1 &&
                                   now - s_last_tap_us <= 520000;
        if (s_second_press_candidate) cancel_tap_timer();
        return;
    }
    if (code == LV_EVENT_LONG_PRESSED) {
        if (s_second_press_candidate &&
            s_assistant_state.load() == ASSISTANT_IDLE && s_voice_active.load()) {
            s_tap_count = 0;
            cancel_tap_timer();
            s_hold_touch_active = true;
            kage_bridge_hold_start();
            wake_up(true);
        }
        s_second_press_candidate = false;
        return;
    }
    if (code == LV_EVENT_RELEASED) {
        if (s_hold_touch_active) {
            s_hold_touch_active = false;
            s_tap_count = 0;
            cancel_tap_timer();
            kage_bridge_hold_stop();
        }
        return;
    }
    if (code != LV_EVENT_SHORT_CLICKED) return;
    s_second_press_candidate = false;
    const int64_t now = esp_timer_get_time();
    if (s_angry) {
        s_angry = false;
        s_tap_count = 0;
        cancel_tap_timer();
        wake_up(false);
        return;
    }
    if (now - s_last_tap_us > 520000) {
        s_tap_count = 0;
        cancel_tap_timer();
    }
    s_last_tap_us = now;
    ++s_tap_count;
    wake_up(true);
    if (s_tap_count >= 3) {
        // A third tap wins over the pending double-tap action.
        cancel_tap_timer();
        kage_bridge_toggle_voice();
        s_tap_count = 0;
        return;
    }

    // Defer the double-tap action long enough to let a third rapid tap arrive.
    // Without this timer, tap two consumes the gesture and triple tap is impossible.
    cancel_tap_timer();
    s_tap_timer = lv_timer_create(complete_tap_gesture, 560, nullptr);
    lv_timer_set_repeat_count(s_tap_timer, 1);
}

static void set_geometry(lv_obj_t *eye, int x, int y, int width, int height) {
    if (lv_obj_get_x(eye) != x || lv_obj_get_y(eye) != y) lv_obj_set_pos(eye, x, y);
    if (lv_obj_get_width(eye) != width || lv_obj_get_height(eye) != height) {
        lv_obj_set_size(eye, width, height);
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
        lv_obj_set_style_bg_color(s_thinking_dots[i], lv_color_hex(s_angry ? RED : PURPLE), 0);
        lv_obj_set_style_opa(s_thinking_dots[i], LV_OPA_COVER, 0);
    }
}

static void update_voice_indicator() {
    const bool active = s_voice_active.load();
    if (!active) {
        lv_obj_set_style_opa(s_voice_indicator, LV_OPA_TRANSP, 0);
        return;
    }
    const float pulse = 0.35f + 0.65f * (0.5f + 0.5f * sinf(s_time * 4.0f));
    lv_obj_set_style_opa(s_voice_indicator, static_cast<lv_opa_t>(pulse * 255.0f), 0);
}

static void animate(lv_timer_t *) {
    if (!s_active) return;
    const int64_t now = esp_timer_get_time();
    if (now - s_last_frame_us < FRAME_MS * 1000) return;

    float dt = static_cast<float>(now - s_last_frame_us) / 1000000.0f;
    s_last_frame_us = now;
    if (dt > 0.08f) dt = 0.08f;
    s_time += dt;

    // This deadline is evaluated locally: a disconnected M920q cannot leave
    // the preview stuck over the normal face.
    const bool show_typing_preview =
        esp_timer_get_time() < s_typing_preview_until_us.load(std::memory_order_acquire);
    if (s_typing_preview) {
        lv_obj_set_style_opa(s_typing_preview,
                             show_typing_preview ? LV_OPA_COVER : LV_OPA_TRANSP, 0);
        if (show_typing_preview) return;
    }

    const int assistant_state = s_assistant_state.load();
    const int64_t now_us = esp_timer_get_time();
    int behavior = s_behavior.load();
    if (s_behavior_until_us.load() && now_us >= s_behavior_until_us.load()) {
        s_behavior.store(ROBOT_BEHAVIOR_NEUTRAL);
        s_behavior_until_us.store(0);
        behavior = ROBOT_BEHAVIOR_NEUTRAL;
    }
    if (robot_motion_is_dancing()) behavior = ROBOT_BEHAVIOR_CELEBRATE;
    if (s_activity_pending.exchange(false)) wake_up(false);
    if (s_cancel_dizzy_pending.exchange(false)) s_dizzy_until = s_time;
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

    const bool drive_active = now_us - s_drive_updated_us.load() < 500000;
    const bool manual_active = now_us - s_manual_look_updated_us.load() < 1800000;
    const bool action_active = assistant_state != ASSISTANT_IDLE ||
                               behavior != ROBOT_BEHAVIOR_NEUTRAL ||
                               drive_active || manual_active || s_mouth_level.load() > 0;
    if (s_sleep_started >= 0.0f && action_active) wake_up(false);

    if (!s_angry && !action_active && assistant_state == ASSISTANT_IDLE && s_sleep_started < 0.0f && s_time >= s_dizzy_until &&
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
                s_next_blink = 6.0f + random_unit() * 5.0f;
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
            s_blinks_left = 1;
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
    if (now_us - s_manual_look_updated_us.load() < 1800000) {
        gaze_x += s_manual_look_x.load() * 18;
        gaze_y += s_manual_look_y.load() * 12;
    }
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

    switch (behavior) {
        case ROBOT_BEHAVIOR_AFFIRM:
        case ROBOT_BEHAVIOR_HAPPY:
        case ROBOT_BEHAVIOR_SATISFIED:
        case ROBOT_BEHAVIOR_GENTLE:
            left_height = static_cast<int>(left_height * 0.88f);
            right_height = left_height;
            break;
        case ROBOT_BEHAVIOR_DENY:
            gaze_x += static_cast<int>(sinf(s_time * 9.0f) * 10.0f);
            break;
        case ROBOT_BEHAVIOR_THINKING:
        case ROBOT_BEHAVIOR_SARCASTIC:
            gaze_x -= 12;
            left_height = static_cast<int>(left_height * 0.88f);
            break;
        case ROBOT_BEHAVIOR_CONFUSED:
            left_height = static_cast<int>(left_height * 0.78f);
            right_height = static_cast<int>(right_height * 1.04f);
            break;
        case ROBOT_BEHAVIOR_CURIOUS:
            left_height = static_cast<int>(left_height * 1.05f);
            right_height = static_cast<int>(right_height * 0.86f);
            gaze_x += 7;
            break;
        case ROBOT_BEHAVIOR_SURPRISED:
            left_height = static_cast<int>(left_height * 1.08f);
            right_height = left_height;
            left_width += 7;
            right_width += 7;
            break;
        case ROBOT_BEHAVIOR_WORRIED:
            left_height = static_cast<int>(left_height * 0.80f);
            right_height = left_height;
            break;
        case ROBOT_BEHAVIOR_SAD:
            left_height = static_cast<int>(left_height * 0.74f);
            right_height = left_height;
            gaze_y += 5;
            break;
        case ROBOT_BEHAVIOR_CELEBRATE:
        case ROBOT_BEHAVIOR_AMUSED: {
            const int bounce = static_cast<int>(fabsf(sinf(s_time * 7.0f)) * 9.0f);
            left_height = std::max(30, left_height - bounce);
            right_height = left_height;
            break;
        }
        case ROBOT_BEHAVIOR_EXPLORE:
            gaze_x += static_cast<int>(sinf(s_time * 2.2f) * 12.0f);
            break;
        default:
            break;
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
    uint32_t base_face_color = CYAN;
    if (s_angry) {
        base_face_color = RED;
    } else if (behavior == ROBOT_BEHAVIOR_CELEBRATE) {
        // Explicit action colors win over transient assistant states.
        base_face_color = YELLOW;
    } else if (behavior == ROBOT_BEHAVIOR_EXPLORE) {
        base_face_color = WHITE;
    } else if (assistant_state == ASSISTANT_LISTENING) {
        base_face_color = GREEN;
    } else if (assistant_state == ASSISTANT_THINKING) {
        base_face_color = PURPLE;
    } else if (assistant_state == ASSISTANT_ERROR) {
        base_face_color = RED;
    } else if (assistant_state == ASSISTANT_OFFLINE) {
        base_face_color = DIM;
    } else if (behavior == ROBOT_BEHAVIOR_WARNING || behavior == ROBOT_BEHAVIOR_WORRIED) {
        base_face_color = ORANGE;
    } else if (behavior == ROBOT_BEHAVIOR_SAD) {
        base_face_color = 0x68A8FF;
    } else if (behavior == ROBOT_BEHAVIOR_THINKING || behavior == ROBOT_BEHAVIOR_CONFUSED ||
               behavior == ROBOT_BEHAVIOR_SARCASTIC) {
        base_face_color = PURPLE;
    } else if (behavior == ROBOT_BEHAVIOR_HAPPY || behavior == ROBOT_BEHAVIOR_AMUSED ||
               behavior == ROBOT_BEHAVIOR_SATISFIED ||
               behavior == ROBOT_BEHAVIOR_GENTLE) {
        base_face_color = GREEN;
    } else if (behavior == ROBOT_BEHAVIOR_SURPRISED) {
        base_face_color = ORANGE;
    } else if (s_time < s_charge_until) {
        const float remaining = s_charge_until - s_time;
        const float pulse = 0.5f + 0.5f * sinf((CHARGE_DURATION_S - remaining) * 8.0f);
        left_height = static_cast<int>(left_height * (0.92f + pulse * 0.12f));
        right_height = left_height;
        base_face_color = GREEN;
    } else {
        base_face_color = CYAN;
    }
    const int forward = s_drive_forward.load();
    const int turn = s_drive_turn.load();
    if (drive_active) {
        const int speed = std::max(abs(forward), abs(turn));
        if (abs(turn) > 12) gaze_x += turn > 0 ? -18 : 18;
        if (forward > 12) gaze_y -= 7;
        else if (forward < -12) {
            gaze_y += 10;
            if (!s_angry && behavior != ROBOT_BEHAVIOR_CELEBRATE &&
                behavior != ROBOT_BEHAVIOR_EXPLORE) base_face_color = ORANGE;
        }
        if (speed > 35) {
            const float focus = 1.0f - std::min(0.18f, (speed - 35) / 360.0f);
            left_height = std::max(34, static_cast<int>(left_height * focus));
            right_height = std::max(34, static_cast<int>(right_height * focus));
        }
    }
    const int center_y = SCREEN_H / 2 + static_cast<int>(bob) + gaze_y;
    const bool custom_face = s_custom_face_active.load(std::memory_order_acquire);
    if (custom_face) base_face_color = static_cast<uint32_t>(s_custom_face[0].load());
    set_face_color(base_face_color);
    update_speed_lines(drive_active && forward > 12, std::max(0, forward), center_y, base_face_color);
    const int left_center_x = SCREEN_W / 2 - EYE_OFFSET_X + gaze_x;
    const int right_center_x = SCREEN_W / 2 + EYE_OFFSET_X + gaze_x;
    // Angry reuses the standard rounded eyes. Small black overlays lower the
    // inner top corners without rotating or rebuilding the eye objects.
    if (s_angry) {
        left_height = std::max(38, left_height * 92 / 100);
        right_height = left_height;
        gaze_y += 2;
    }
    set_geometry(s_left_eye, left_center_x - left_width / 2, center_y - left_height / 2,
                 left_width, left_height);
    set_geometry(s_right_eye, right_center_x - right_width / 2, center_y - right_height / 2,
                 right_width, right_height);
    s_custom_left_x = left_center_x;
    s_custom_right_x = right_center_x;
    s_custom_center_y = center_y;
    s_custom_left_w = left_width;
    s_custom_right_w = right_width;
    s_custom_left_h = left_height;
    s_custom_right_h = right_height;
    lv_obj_set_style_opa(s_left_eye, custom_face ? LV_OPA_TRANSP : LV_OPA_COVER, 0);
    lv_obj_set_style_opa(s_right_eye, custom_face ? LV_OPA_TRANSP : LV_OPA_COVER, 0);
    lv_obj_invalidate(s_custom_eye_layer);
    const bool angry_brows = s_angry && !custom_face;
    const int brow_y = center_y - left_height / 2 - 1;
    set_geometry(s_angry_left_brow,
                 left_center_x + left_width / 2 - 21, brow_y, 23, 14);
    set_geometry(s_angry_right_brow,
                 right_center_x - right_width / 2 - 2, brow_y, 23, 14);
    lv_obj_set_style_opa(s_angry_left_brow, angry_brows ? LV_OPA_COVER : LV_OPA_TRANSP, 0);
    lv_obj_set_style_opa(s_angry_right_brow, angry_brows ? LV_OPA_COVER : LV_OPA_TRANSP, 0);
    int mouth_w = MOUTH_W + static_cast<int>(bob * 0.5f);
    int mouth_h = MOUTH_H;
    int mouth_y = MOUTH_Y + static_cast<int>(bob);
    int mouth_x = SCREEN_W / 2 - mouth_w / 2;
    int mouth_rotation = 0;
    if (s_sleep_started >= 0.0f) mouth_w = 34;
    if (s_expression == 1) mouth_rotation = -45;
    if (s_expression == 2) mouth_w = 62;
    if (behavior == ROBOT_BEHAVIOR_SURPRISED) { mouth_w = 18; mouth_h = 16; }
    if (behavior == ROBOT_BEHAVIOR_CONFUSED) mouth_w = 24;
    if (behavior == ROBOT_BEHAVIOR_AMUSED || behavior == ROBOT_BEHAVIOR_HAPPY ||
        behavior == ROBOT_BEHAVIOR_CELEBRATE || behavior == ROBOT_BEHAVIOR_SATISFIED ||
        behavior == ROBOT_BEHAVIOR_GENTLE) mouth_w = 62;
    if (behavior == ROBOT_BEHAVIOR_SAD || behavior == ROBOT_BEHAVIOR_WORRIED) mouth_w = 40;
    if (s_angry) mouth_w = 54;
    if (custom_face) {
        mouth_w = s_custom_face[4].load();
        mouth_h = s_custom_face[5].load();
        mouth_y = s_custom_face[6].load();
        mouth_x = SCREEN_W / 2 - mouth_w / 2;
    }
    if (s_time < s_dizzy_until) {
        const float phase = (DIZZY_DURATION_S - (s_dizzy_until - s_time)) * 11.0f;
        mouth_w = 52;
        mouth_x = SCREEN_W / 2 - mouth_w / 2 + static_cast<int>(sinf(phase * 0.7f) * 5.0f);
        mouth_y += static_cast<int>(cosf(phase * 0.8f) * 4.0f);
        mouth_rotation = static_cast<int>(sinf(phase * 0.55f) * 120.0f);
    }
    const bool dance_smile = custom_face ? s_custom_face[15].load() == 1 :
                             (!s_angry && behavior == ROBOT_BEHAVIOR_CELEBRATE);
    lv_obj_set_style_arc_color(s_smile, lv_color_hex(custom_face ? base_face_color : YELLOW),
                               LV_PART_INDICATOR);
    lv_obj_set_style_opa(s_mouth, dance_smile ? LV_OPA_TRANSP : LV_OPA_COVER, 0);
    lv_obj_set_style_opa(s_smile, dance_smile ? LV_OPA_COVER : LV_OPA_TRANSP, 0);
    if (dance_smile) {
        const int bounce = static_cast<int>(fabsf(sinf(s_time * 7.0f)) * 3.0f);
        lv_obj_set_pos(s_smile, SCREEN_W / 2 - 29, 212 - bounce);
    }
    static float mouth_smoothed = 0.0f;
    const int level = s_mouth_level.load();
    const float mouth_target = static_cast<float>(level) / 1000.0f;
    // Follow speech envelope quickly; a slow release would visibly lag behind
    // the TTS and make the mouth keep moving after audio has stopped.
    const float mouth_rate = mouth_target > mouth_smoothed ? 1.0f : 0.94f;
    mouth_smoothed += (mouth_target - mouth_smoothed) * mouth_rate;
    const bool voice_speaking = assistant_state == ASSISTANT_SPEAKING ||
                                s_voice_active.load() || level > 20;
    if (!dance_smile && voice_speaking && level > 8) {
        // The PC envelope arrives roughly every 80-100 ms. Add a fast
        // syllable pulse while real audio is present so the mouth articulates
        // several times per spoken word without moving after TTS stops.
        const float chatter = 0.30f + 0.70f * fabsf(sinf(s_time * 25.0f));
        const float amplified = std::min(1.0f, sqrtf(std::max(0.0f, mouth_smoothed)) * 1.45f);
        const float voice_pulse = amplified * chatter;
        set_geometry(s_mouth, mouth_x, mouth_y - static_cast<int>(voice_pulse * 11.0f),
                     mouth_w, mouth_h + static_cast<int>(voice_pulse * 22.0f));
    } else {
        set_geometry(s_mouth, mouth_x, mouth_y, mouth_w, mouth_h);
    }
    lv_obj_set_style_transform_rotation(s_mouth, mouth_rotation, 0);
    update_sleep_marks(static_cast<float>(center_y));
    update_thinking_dots(assistant_state);
    update_charge_bolt();
    update_voice_indicator();
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

static lv_obj_t *create_speed_line(lv_obj_t *parent) {
    lv_obj_t *line = lv_obj_create(parent);
    lv_obj_remove_style_all(line);
    lv_obj_set_style_bg_opa(line, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(line, lv_color_hex(CYAN), 0);
    lv_obj_set_style_radius(line, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_opa(line, LV_OPA_TRANSP, 0);
    lv_obj_clear_flag(line, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_clear_flag(line, LV_OBJ_FLAG_SCROLLABLE);
    return line;
}

static lv_obj_t *create_angry_brow(lv_obj_t *parent) {
    lv_obj_t *brow = lv_obj_create(parent);
    lv_obj_remove_style_all(brow);
    lv_obj_set_style_bg_color(brow, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(brow, LV_OPA_COVER, 0);
    lv_obj_set_style_opa(brow, LV_OPA_TRANSP, 0);
    lv_obj_clear_flag(brow, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_clear_flag(brow, LV_OBJ_FLAG_SCROLLABLE);
    return brow;
}

static lv_obj_t *create_smile(lv_obj_t *parent) {
    lv_obj_t *smile = lv_arc_create(parent);
    lv_obj_remove_style_all(smile);
    lv_obj_set_size(smile, 58, 58);
    lv_arc_set_rotation(smile, 0);
    lv_arc_set_bg_angles(smile, 0, 0);
    lv_arc_set_angles(smile, 42, 138);
    lv_obj_set_style_arc_opa(smile, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_arc_width(smile, 8, LV_PART_INDICATOR);
    lv_obj_set_style_arc_rounded(smile, true, LV_PART_INDICATOR);
    lv_obj_set_style_arc_color(smile, lv_color_hex(YELLOW), LV_PART_INDICATOR);
    lv_obj_set_style_arc_opa(smile, LV_OPA_COVER, LV_PART_INDICATOR);
    lv_obj_set_style_opa(smile, LV_OPA_TRANSP, LV_PART_KNOB);
    lv_obj_clear_flag(smile, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_clear_flag(smile, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_opa(smile, LV_OPA_TRANSP, 0);
    return smile;
}
}  // namespace

void robot_eyes_set_custom_face(const int *values, size_t count, bool enabled) {
    s_custom_face_active.store(false, std::memory_order_release);
    if (!enabled || !values || count != 16) return;
    for (size_t i = 0; i < 16; ++i)
        s_custom_face[i].store(values[i], std::memory_order_relaxed);
    s_custom_face_active.store(true, std::memory_order_release);
}

void robot_eyes_preview_typing_layout(int duration_ms) {
    duration_ms = std::max(1000, std::min(60000, duration_ms));
    s_typing_preview_until_us.store(esp_timer_get_time() +
                                    static_cast<int64_t>(duration_ms) * 1000,
                                    std::memory_order_release);
}

void robot_eyes_begin(lv_obj_t *parent) {
    lv_obj_set_style_bg_color(parent, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(parent, LV_OPA_COVER, 0);
    lv_obj_clear_flag(parent, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(parent, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(parent, touch_event, LV_EVENT_SHORT_CLICKED, nullptr);
    lv_obj_add_event_cb(parent, touch_event, LV_EVENT_PRESSED, nullptr);
    lv_obj_add_event_cb(parent, touch_event, LV_EVENT_LONG_PRESSED, nullptr);
    lv_obj_add_event_cb(parent, touch_event, LV_EVENT_RELEASED, nullptr);

    s_left_eye = create_eye(parent);
    s_right_eye = create_eye(parent);
    s_custom_eye_layer = lv_obj_create(parent);
    lv_obj_remove_style_all(s_custom_eye_layer);
    lv_obj_set_size(s_custom_eye_layer, SCREEN_W, SCREEN_H);
    lv_obj_set_pos(s_custom_eye_layer, 0, 0);
    lv_obj_clear_flag(s_custom_eye_layer, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_clear_flag(s_custom_eye_layer, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(s_custom_eye_layer, draw_custom_eyes, LV_EVENT_DRAW_MAIN, nullptr);
    s_mouth = create_mouth(parent);
    s_smile = create_smile(parent);
    s_angry_left_brow = create_angry_brow(parent);
    s_angry_right_brow = create_angry_brow(parent);
    for (auto &line : s_speed_lines) line = create_speed_line(parent);
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
    lv_obj_clear_flag(s_left_eye, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_clear_flag(s_right_eye, LV_OBJ_FLAG_CLICKABLE);
    set_geometry(s_left_eye, SCREEN_W / 2 - EYE_OFFSET_X - EYE_W / 2, SCREEN_H / 2 - 2, EYE_W, 4);
    set_geometry(s_right_eye, SCREEN_W / 2 + EYE_OFFSET_X - EYE_W / 2, SCREEN_H / 2 - 2, EYE_W, 4);
    set_geometry(s_mouth, SCREEN_W / 2 - MOUTH_W / 2, MOUTH_Y, MOUTH_W, MOUTH_H);

    s_voice_indicator = lv_obj_create(parent);
    lv_obj_remove_style_all(s_voice_indicator);
    lv_obj_set_style_bg_color(s_voice_indicator, lv_color_hex(ORANGE), 0);
    lv_obj_set_style_bg_opa(s_voice_indicator, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(s_voice_indicator, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_size(s_voice_indicator, 14, 14);
    lv_obj_set_pos(s_voice_indicator, 24, 26);
    lv_obj_clear_flag(s_voice_indicator, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_opa(s_voice_indicator, LV_OPA_TRANSP, 0);

    create_typing_preview(parent);

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
void robot_eyes_set_voice_active(bool active) { s_voice_active.store(active); }
void robot_eyes_note_activity() { s_activity_pending.store(true); }
void robot_eyes_cancel_dizzy() { s_cancel_dizzy_pending.store(true); }
void robot_eyes_set_mouth_level(int level) {
    if (level < 0) level = 0;
    if (level > 1000) level = 1000;
    s_mouth_level.store(level);
    if (level > 0) robot_eyes_note_activity();
}
void robot_eyes_set_drive_state(int forward_percent, int turn_percent) {
    s_drive_forward.store(std::max(-100, std::min(100, forward_percent)));
    s_drive_turn.store(std::max(-100, std::min(100, turn_percent)));
    s_drive_updated_us.store((forward_percent || turn_percent) ? esp_timer_get_time() : 0);
    if (forward_percent || turn_percent) robot_eyes_note_activity();
}
void robot_eyes_set_manual_look(int horizontal, int vertical) {
    horizontal = std::max(-1, std::min(1, horizontal));
    vertical = std::max(-1, std::min(1, vertical));
    s_manual_look_x.store(horizontal);
    s_manual_look_y.store(vertical);
    s_manual_look_updated_us.store((horizontal || vertical) ? esp_timer_get_time() : 0);
    if (horizontal || vertical) robot_eyes_note_activity();
}
void robot_eyes_set_behavior(int behavior, int duration_ms) {
    if (behavior < ROBOT_BEHAVIOR_NEUTRAL || behavior > ROBOT_BEHAVIOR_EXPLORE) return;
    duration_ms = std::max(100, std::min(20000, duration_ms));
    s_behavior.store(behavior);
    s_behavior_until_us.store(esp_timer_get_time() + static_cast<int64_t>(duration_ms) * 1000);
    if (behavior != ROBOT_BEHAVIOR_NEUTRAL) robot_eyes_note_activity();
}
