#include "app_shell.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>

#include "audio/mic_meter.h"
#include "battery_monitor.h"
#include "bsp/esp-bsp.h"
#include "esp_heap_caps.h"
#include "esp_idf_version.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "eyes/robot_eyes.h"
#include "services/orientation_service.h"
#include "services/event_log.h"
#include "services/kage_bridge.h"
#include "services/ota_service.h"
#include "services/wifi_service.h"

#ifndef LV_SYMBOL_EYE_OPEN
#define LV_SYMBOL_EYE_OPEN "o o"
#endif

namespace {
constexpr int SCREEN_W = 448;
constexpr int SCREEN_H = 368;
constexpr uint32_t COLOR_BG = 0x05080B;
constexpr uint32_t COLOR_SURFACE = 0x111820;
constexpr uint32_t COLOR_SURFACE_2 = 0x18232D;
constexpr uint32_t COLOR_BORDER = 0x2A3A46;
constexpr uint32_t COLOR_TEXT = 0xEAFBFF;
constexpr uint32_t COLOR_MUTED = 0x78909C;
constexpr uint32_t COLOR_CYAN = 0x4FE3FF;
constexpr uint32_t COLOR_ORB = 0xA78BFA;
constexpr uint32_t COLOR_RED = 0xFF5964;
constexpr int BUBBLE_COUNT = 7;
constexpr int MIC_DOT_COUNT = 42;
constexpr float PI = 3.14159265358979323846f;

enum AppId {
    APP_ROBOT,
    APP_MIC,
    APP_MOTION,
    APP_DISPLAY,
    APP_SYSTEM,
    APP_WIFI,
    APP_STORAGE,
};

struct BubbleSpec {
    const char *symbol;
    float x;
    float y;
    float diameter;
    uint32_t color;
};

/* A compact hexagonal cluster: regular geometry makes the magnetic deflection
   legible while leaving generous touch targets. */
constexpr BubbleSpec BUBBLES[BUBBLE_COUNT] = {
    {LV_SYMBOL_EYE_OPEN, 224, 174, 108, 0x2583FF},
    {LV_SYMBOL_AUDIO, 224, 55, 76, 0x9B5DE5},
    {LV_SYMBOL_REFRESH, 330, 112, 76, 0xFF7A45},
    {LV_SYMBOL_IMAGE, 330, 236, 76, 0x20C997},
    {LV_SYMBOL_SETTINGS, 224, 303, 76, 0xF4B942},
    {LV_SYMBOL_WIFI, 118, 236, 76, 0x36C5F0},
    {LV_SYMBOL_SD_CARD, 118, 112, 76, 0xEF5DA8},
};

struct BubbleRuntime {
    lv_obj_t *body;
    lv_obj_t *icon;
    float x;
    float y;
    float scale;
    float vx;
    float vy;
    float vs;
};

static lv_display_t *s_display;
static lv_obj_t *s_home;
static lv_obj_t *s_robot;
static lv_obj_t *s_mic;
static lv_obj_t *s_motion;
static lv_obj_t *s_display_app;
static lv_obj_t *s_system;
static lv_obj_t *s_wifi;
static lv_obj_t *s_storage;
static BubbleRuntime s_bubbles[BUBBLE_COUNT];
static AppId s_current = APP_ROBOT;
static int s_selected;
static int s_pending_open = -1;
static int64_t s_open_at_us;
static int64_t s_home_last_us;

struct MicDotSeed {
    float angle;
    float radius;
    float phase;
    float drift;
};

static lv_obj_t *s_mic_dots[MIC_DOT_COUNT];
static MicDotSeed s_mic_seed[MIC_DOT_COUNT];
static lv_obj_t *s_mic_status;
static float s_mic_phase;

static lv_obj_t *s_motion_values;
static lv_obj_t *s_motion_ball;
static lv_obj_t *s_motion_calibration_status;
static lv_obj_t *s_system_values;
static lv_obj_t *s_ota_status;
static lv_obj_t *s_wifi_values;
static lv_obj_t *s_wifi_logs;
static lv_obj_t *s_wifi_logs_values;
static lv_obj_t *s_storage_status;
static lv_obj_t *s_brightness_value;
static bool s_sd_mounted;
static bool s_have_charge_state;
static bool s_last_charging;

static float clampf(float value, float low, float high) {
    return std::max(low, std::min(high, value));
}

static void clear_default_screen(lv_obj_t *screen) {
    lv_obj_remove_style_all(screen);
    lv_obj_set_size(screen, SCREEN_W, SCREEN_H);
    lv_obj_set_style_bg_color(screen, lv_color_hex(COLOR_BG), 0);
    lv_obj_set_style_bg_opa(screen, LV_OPA_COVER, 0);
    lv_obj_clear_flag(screen, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(screen, LV_OBJ_FLAG_CLICKABLE);
}

static lv_obj_t *make_label(lv_obj_t *parent, const char *text, const lv_font_t *font,
                            uint32_t color) {
    lv_obj_t *label = lv_label_create(parent);
    lv_label_set_text(label, text);
    lv_obj_set_style_text_font(label, font, 0);
    lv_obj_set_style_text_color(label, lv_color_hex(color), 0);
    lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_CENTER, 0);
    return label;
}

static lv_obj_t *make_card(lv_obj_t *parent, int x, int y, int width, int height) {
    lv_obj_t *card = lv_obj_create(parent);
    lv_obj_remove_style_all(card);
    lv_obj_set_pos(card, x, y);
    lv_obj_set_size(card, width, height);
    lv_obj_set_style_bg_color(card, lv_color_hex(COLOR_SURFACE), 0);
    lv_obj_set_style_bg_opa(card, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(card, lv_color_hex(COLOR_BORDER), 0);
    lv_obj_set_style_border_width(card, 1, 0);
    lv_obj_set_style_radius(card, 22, 0);
    lv_obj_clear_flag(card, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(card, LV_OBJ_FLAG_GESTURE_BUBBLE);
    return card;
}

static lv_obj_t *create_app_screen(const char *title, const char *subtitle) {
    lv_obj_t *screen = lv_obj_create(nullptr);
    clear_default_screen(screen);
    lv_obj_t *heading = make_label(screen, title, &lv_font_montserrat_24, COLOR_TEXT);
    lv_obj_set_pos(heading, 28, 22);
    if (subtitle) {
        lv_obj_t *sub = make_label(screen, subtitle, &lv_font_montserrat_14, COLOR_MUTED);
        lv_obj_set_style_text_align(sub, LV_TEXT_ALIGN_LEFT, 0);
        lv_obj_set_width(sub, 392);
        lv_obj_set_pos(sub, 28, 54);
    }
    return screen;
}

static void set_bubble_appearance(int index) {
    const bool selected = index == s_selected;
    lv_obj_t *body = s_bubbles[index].body;
    lv_obj_set_style_bg_color(body, lv_color_hex(BUBBLES[index].color), 0);
    lv_obj_set_style_border_color(body, lv_color_white(), 0);
    lv_obj_set_style_border_width(body, selected ? 3 : 0, 0);
}

static lv_obj_t *create_robot_home_icon(lv_obj_t *parent) {
    lv_obj_t *layer = lv_obj_create(parent);
    lv_obj_remove_style_all(layer);
    lv_obj_set_size(layer, 50, 42);
    lv_obj_clear_flag(layer, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_clear_flag(layer, LV_OBJ_FLAG_SCROLLABLE);
    for (int i = 0; i < 2; ++i) {
        const int x = i == 0 ? 4 : 28;
        lv_obj_t *eye = lv_obj_create(layer);
        lv_obj_remove_style_all(eye);
        lv_obj_set_pos(eye, x, 5);
        lv_obj_set_size(eye, 18, 32);
        lv_obj_set_style_bg_color(eye, lv_color_white(), 0);
        lv_obj_set_style_bg_opa(eye, LV_OPA_COVER, 0);
        lv_obj_set_style_radius(eye, 8, 0);
        lv_obj_clear_flag(eye, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_clear_flag(eye, LV_OBJ_FLAG_SCROLLABLE);
    }
    return layer;
}

static void select_bubble(int index) {
    if (index < 0) index = BUBBLE_COUNT - 1;
    if (index >= BUBBLE_COUNT) index = 0;
    const int previous = s_selected;
    s_selected = index;
    set_bubble_appearance(previous);
    set_bubble_appearance(s_selected);
}

static void set_app_activity(AppId app) {
    robot_eyes_set_active(app == APP_ROBOT);
    mic_meter_set_active(app == APP_MIC);
}

static lv_obj_t *screen_for(AppId app) {
    switch (app) {
        case APP_ROBOT: return s_robot;
        case APP_MIC: return s_mic;
        case APP_MOTION: return s_motion;
        case APP_DISPLAY: return s_display_app;
        case APP_SYSTEM: return s_system;
        case APP_WIFI: return s_wifi;
        case APP_STORAGE: return s_storage;
    }
    return s_home;
}

static void open_app(AppId app) {
    s_pending_open = -1;
    s_current = app;
    set_app_activity(app);
    lv_screen_load_anim(screen_for(app), LV_SCR_LOAD_ANIM_FADE_IN, 180, 0, false);
}

static void show_home() {
    s_pending_open = -1;
    set_app_activity(static_cast<AppId>(-1));
    select_bubble(static_cast<int>(s_current));
    lv_screen_load_anim(s_home, LV_SCR_LOAD_ANIM_MOVE_BOTTOM, 220, 0, false);
}

static void gesture_event(lv_event_t *) {
    lv_indev_t *input = lv_indev_active();
    if (!input) return;
    const lv_dir_t direction = lv_indev_get_gesture_dir(input);
    if (lv_screen_active() == s_home) {
        if (direction == LV_DIR_LEFT) select_bubble(s_selected + 1);
        else if (direction == LV_DIR_RIGHT) select_bubble(s_selected - 1);
    } else if (direction == LV_DIR_TOP) {
        show_home();
    }
}

static void bubble_pressed(lv_event_t *event) {
    const int index = static_cast<int>(reinterpret_cast<intptr_t>(lv_event_get_user_data(event)));
    select_bubble(index);
    s_pending_open = index;
    s_open_at_us = esp_timer_get_time() + 190000;
}

static void bubble_targets(int index, float *target_x, float *target_y, float *target_scale) {
    const BubbleSpec &bubble = BUBBLES[index];
    *target_x = bubble.x;
    *target_y = bubble.y;
    *target_scale = index == s_selected ? 1.16f : 0.96f;
    if (index == s_selected) return;

    const BubbleSpec &magnet = BUBBLES[s_selected];
    const float dx = bubble.x - magnet.x;
    const float dy = bubble.y - magnet.y;
    const float distance = std::sqrt(dx * dx + dy * dy);
    if (distance < 1.0f) return;
    const float push = 5.0f + 7.0f * std::exp(-distance / 170.0f);
    *target_x += dx / distance * push;
    *target_y += dy / distance * push;
}

static void home_animation(lv_timer_t *) {
    const int64_t now = esp_timer_get_time();
    float frame = s_home_last_us ? static_cast<float>(now - s_home_last_us) / 16667.0f : 1.0f;
    s_home_last_us = now;
    frame = clampf(frame, 0.35f, 2.5f);
    const float decay = std::pow(0.70f, frame);

    for (int i = 0; i < BUBBLE_COUNT; ++i) {
        float tx, ty, ts;
        bubble_targets(i, &tx, &ty, &ts);
        BubbleRuntime &bubble = s_bubbles[i];
        bubble.vx = (bubble.vx + (tx - bubble.x) * 0.15f * frame) * decay;
        bubble.vy = (bubble.vy + (ty - bubble.y) * 0.15f * frame) * decay;
        bubble.vs = (bubble.vs + (ts - bubble.scale) * 0.18f * frame) * decay;
        bubble.x += bubble.vx * frame;
        bubble.y += bubble.vy * frame;
        bubble.scale += bubble.vs * frame;

        const int diameter = static_cast<int>(BUBBLES[i].diameter * bubble.scale + 0.5f);
        lv_obj_set_size(bubble.body, diameter, diameter);
        lv_obj_set_pos(bubble.body, static_cast<int>(bubble.x - diameter / 2.0f),
                       static_cast<int>(bubble.y - diameter / 2.0f));
        lv_obj_align(bubble.icon, LV_ALIGN_CENTER, 0, 0);
    }

    if (s_pending_open >= 0 && now >= s_open_at_us) {
        open_app(static_cast<AppId>(s_pending_open));
    }
}

static void create_home_screen() {
    s_home = lv_obj_create(nullptr);
    clear_default_screen(s_home);
    lv_obj_add_event_cb(s_home, gesture_event, LV_EVENT_GESTURE, nullptr);

    for (int i = 0; i < BUBBLE_COUNT; ++i) {
        const BubbleSpec &spec = BUBBLES[i];
        BubbleRuntime &runtime = s_bubbles[i];
        float tx, ty, ts;
        bubble_targets(i, &tx, &ty, &ts);
        runtime.x = tx;
        runtime.y = ty;
        runtime.scale = ts;
        runtime.body = lv_obj_create(s_home);
        lv_obj_remove_style_all(runtime.body);
        lv_obj_set_style_bg_opa(runtime.body, LV_OPA_COVER, 0);
        lv_obj_set_style_radius(runtime.body, LV_RADIUS_CIRCLE, 0);
        lv_obj_clear_flag(runtime.body, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_flag(runtime.body, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_flag(runtime.body, LV_OBJ_FLAG_GESTURE_BUBBLE);
        lv_obj_add_event_cb(runtime.body, bubble_pressed, LV_EVENT_SHORT_CLICKED,
                            reinterpret_cast<void *>(static_cast<intptr_t>(i)));
        runtime.icon = i == APP_ROBOT
            ? create_robot_home_icon(runtime.body)
            : make_label(runtime.body, spec.symbol, &lv_font_montserrat_24, 0xFFFFFF);
        lv_obj_clear_flag(runtime.icon, LV_OBJ_FLAG_CLICKABLE);
        set_bubble_appearance(i);
    }
    lv_timer_create(home_animation, 16, nullptr);
}

static void create_robot_screen() {
    s_robot = lv_obj_create(nullptr);
    clear_default_screen(s_robot);
    lv_obj_add_event_cb(s_robot, gesture_event, LV_EVENT_GESTURE, nullptr);
    robot_eyes_begin(s_robot);
    robot_eyes_set_active(false);
}

static void microphone_animation(lv_timer_t *) {
    if (s_current != APP_MIC || lv_screen_active() != s_mic) return;
    const float level = mic_meter_level();
    s_mic_phase += 0.045f + level * 0.09f;
    const float breathe = 0.5f + 0.5f * std::sin(s_mic_phase * 0.72f);
    for (int i = 0; i < MIC_DOT_COUNT; ++i) {
        const MicDotSeed &seed = s_mic_seed[i];
        const float angle = seed.angle + 0.10f * std::sin(s_mic_phase * seed.drift + seed.phase);
        const float skin = 1.0f + 0.10f * std::sin(angle * 3.0f + s_mic_phase * 0.82f) +
                           0.055f * std::sin(angle * 5.0f - s_mic_phase * 0.53f);
        const float voice = level * (10.0f + 24.0f * seed.radius) *
                            (0.55f + 0.45f * std::sin(seed.phase + s_mic_phase * 2.3f));
        const float radius = seed.radius * (72.0f + breathe * 6.0f) * skin + voice;
        const float grain = 0.5f + 0.5f * std::sin(seed.phase + s_mic_phase * 1.7f);
        const int size = static_cast<int>(4.0f + seed.radius * 3.0f + level * 5.0f * grain);
        const int x = 224 + static_cast<int>(std::cos(angle) * radius) - size / 2;
        const int y = 174 + static_cast<int>(std::sin(angle) * radius * 0.94f) - size / 2;
        lv_obj_set_pos(s_mic_dots[i], x, y);
        lv_obj_set_size(s_mic_dots[i], size, size);
        const int opacity = static_cast<int>(105.0f + seed.radius * 90.0f + level * 60.0f * grain);
        lv_obj_set_style_opa(s_mic_dots[i], static_cast<lv_opa_t>(std::min(255, opacity)), 0);
    }

    const MicMeterState state = mic_meter_state();
    const char *status = "OFF";
    uint32_t color = COLOR_MUTED;
    if (state == MicMeterState::Starting) status = "STARTING";
    else if (state == MicMeterState::Listening) { status = "LISTENING"; color = COLOR_CYAN; }
    else if (state == MicMeterState::Error) { status = "MIC ERROR"; color = COLOR_RED; }
    lv_label_set_text_fmt(s_mic_status, "%s  %d%%", status, static_cast<int>(level * 100.0f));
    lv_obj_set_style_text_color(s_mic_status, lv_color_hex(color), 0);
}

static void create_microphone_screen() {
    s_mic = create_app_screen("Microphone", nullptr);
    lv_obj_add_event_cb(s_mic, gesture_event, LV_EVENT_GESTURE, nullptr);
    uint32_t random = 0x4B414745u;
    for (int i = 0; i < MIC_DOT_COUNT; ++i) {
        random = random * 1664525u + 1013904223u;
        const float a = static_cast<float>(random & 0xFFFFu) / 65535.0f;
        random = random * 1664525u + 1013904223u;
        const float r = static_cast<float>(random & 0xFFFFu) / 65535.0f;
        random = random * 1664525u + 1013904223u;
        const float p = static_cast<float>(random & 0xFFFFu) / 65535.0f;
        s_mic_seed[i] = {a * 2.0f * PI, std::sqrt(r) * 0.96f,
                         p * 2.0f * PI, 0.55f + p * 0.65f};
        s_mic_dots[i] = lv_obj_create(s_mic);
        lv_obj_remove_style_all(s_mic_dots[i]);
        lv_obj_set_size(s_mic_dots[i], 5, 5);
        lv_obj_set_style_bg_color(s_mic_dots[i], lv_color_hex(COLOR_ORB), 0);
        lv_obj_set_style_bg_opa(s_mic_dots[i], LV_OPA_COVER, 0);
        lv_obj_set_style_radius(s_mic_dots[i], LV_RADIUS_CIRCLE, 0);
        lv_obj_clear_flag(s_mic_dots[i], LV_OBJ_FLAG_CLICKABLE);
    }
    s_mic_status = make_label(s_mic, "OFF  0%", &lv_font_montserrat_14, COLOR_MUTED);
    lv_obj_align(s_mic_status, LV_ALIGN_BOTTOM_MID, 0, -20);
    lv_timer_create(microphone_animation, 33, nullptr);
}

static void motion_animation(lv_timer_t *) {
    if (s_current != APP_MOTION || lv_screen_active() != s_motion) return;
    float x, y, z;
    if (!orientation_service_get_sample(&x, &y, &z)) {
        lv_label_set_text(s_motion_values, "Waiting for QMI8658...");
        return;
    }
    lv_label_set_text_fmt(s_motion_values, "X  %+.2f\nY  %+.2f\nZ  %+.2f m/s2", x, y, z);
    const int bx = 224 + static_cast<int>(clampf(x / 9.807f, -1.0f, 1.0f) * 48.0f) - 10;
    const int by = 196 + static_cast<int>(clampf(z / 9.807f, -1.0f, 1.0f) * 48.0f) - 10;
    lv_obj_set_pos(s_motion_ball, bx, by);
}

static void calibrate_motion(lv_event_t *) {
    if (orientation_service_calibrate()) {
        lv_label_set_text(s_motion_calibration_status, "CALIBRATED");
        lv_obj_set_style_text_color(s_motion_calibration_status, lv_color_hex(0x20C997), 0);
    } else {
        lv_label_set_text(s_motion_calibration_status, "SENSOR NOT READY");
        lv_obj_set_style_text_color(s_motion_calibration_status, lv_color_hex(COLOR_RED), 0);
    }
}

static void create_motion_screen() {
    s_motion = create_app_screen("Motion", "Place the board flat, then calibrate.");
    lv_obj_add_event_cb(s_motion, gesture_event, LV_EVENT_GESTURE, nullptr);
    lv_obj_t *field = lv_obj_create(s_motion);
    lv_obj_remove_style_all(field);
    lv_obj_set_size(field, 132, 132);
    lv_obj_set_pos(field, 158, 130);
    lv_obj_set_style_bg_color(field, lv_color_hex(COLOR_SURFACE), 0);
    lv_obj_set_style_bg_opa(field, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(field, lv_color_hex(COLOR_BORDER), 0);
    lv_obj_set_style_border_width(field, 2, 0);
    lv_obj_set_style_radius(field, LV_RADIUS_CIRCLE, 0);
    s_motion_ball = lv_obj_create(s_motion);
    lv_obj_remove_style_all(s_motion_ball);
    lv_obj_set_size(s_motion_ball, 20, 20);
    lv_obj_set_style_bg_color(s_motion_ball, lv_color_hex(COLOR_CYAN), 0);
    lv_obj_set_style_bg_opa(s_motion_ball, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(s_motion_ball, LV_RADIUS_CIRCLE, 0);
    s_motion_values = make_label(s_motion, "Waiting for QMI8658...", &lv_font_montserrat_16, COLOR_TEXT);
    lv_obj_set_style_text_align(s_motion_values, LV_TEXT_ALIGN_LEFT, 0);
    lv_obj_set_pos(s_motion_values, 310, 145);
    lv_obj_t *calibrate = lv_button_create(s_motion);
    lv_obj_set_size(calibrate, 170, 48);
    lv_obj_align(calibrate, LV_ALIGN_BOTTOM_MID, 0, -28);
    lv_obj_set_style_bg_color(calibrate, lv_color_hex(0xFF7A45), 0);
    lv_obj_set_style_radius(calibrate, LV_RADIUS_CIRCLE, 0);
    lv_obj_add_flag(calibrate, LV_OBJ_FLAG_GESTURE_BUBBLE);
    lv_obj_add_event_cb(calibrate, calibrate_motion, LV_EVENT_SHORT_CLICKED, nullptr);
    s_motion_calibration_status = make_label(calibrate, "CALIBRATE", &lv_font_montserrat_14, 0xFFFFFF);
    lv_obj_center(s_motion_calibration_status);
    lv_timer_create(motion_animation, 80, nullptr);
}

static void brightness_changed(lv_event_t *event) {
    lv_obj_t *slider = static_cast<lv_obj_t *>(lv_event_get_target(event));
    const int value = lv_slider_get_value(slider);
    bsp_display_brightness_set(value);
    lv_label_set_text_fmt(s_brightness_value, "%d%%", value);
}

static void create_display_screen() {
    s_display_app = create_app_screen("Display", "AMOLED brightness");
    lv_obj_add_event_cb(s_display_app, gesture_event, LV_EVENT_GESTURE, nullptr);
    lv_obj_t *card = make_card(s_display_app, 28, 112, 392, 132);
    lv_obj_t *slider = lv_slider_create(card);
    lv_obj_set_size(slider, 320, 34);
    lv_obj_align(slider, LV_ALIGN_CENTER, 0, 18);
    lv_slider_set_range(slider, 10, 100);
    lv_slider_set_value(slider, 80, LV_ANIM_OFF);
    lv_obj_set_style_bg_color(slider, lv_color_hex(COLOR_SURFACE_2), LV_PART_MAIN);
    lv_obj_set_style_bg_color(slider, lv_color_hex(COLOR_CYAN), LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(slider, lv_color_hex(COLOR_CYAN), LV_PART_KNOB);
    lv_obj_set_style_pad_all(slider, 8, LV_PART_KNOB);
    lv_obj_add_event_cb(slider, brightness_changed, LV_EVENT_VALUE_CHANGED, nullptr);
    s_brightness_value = make_label(card, "80%", &lv_font_montserrat_24, COLOR_TEXT);
    lv_obj_align(s_brightness_value, LV_ALIGN_TOP_MID, 0, 13);
}

static void system_animation(lv_timer_t *) {
    if (s_current != APP_SYSTEM || lv_screen_active() != s_system) return;
    const uint32_t heap_kb = esp_get_free_heap_size() / 1024;
    const uint32_t psram_kb = heap_caps_get_free_size(MALLOC_CAP_SPIRAM) / 1024;
    const uint64_t uptime = esp_timer_get_time() / 1000000ULL;
    float battery = 0.0f;
    bool charging = false;
    char battery_text[40];
    if (battery_monitor_read(&battery, &charging)) {
        std::snprintf(battery_text, sizeof(battery_text), "%d%%  %s",
                      static_cast<int>(battery * 100.0f + 0.5f), charging ? "CHARGING" : "BATTERY");
    } else {
        std::snprintf(battery_text, sizeof(battery_text), "NOT DETECTED");
    }
    lv_label_set_text_fmt(s_system_values,
                          "ESP32-S3  240 MHz\nBattery         %s\nFree memory     %lu KB\nFree PSRAM      %lu KB\nUptime          %llu s\nESP-IDF         %s",
                          battery_text, static_cast<unsigned long>(heap_kb),
                          static_cast<unsigned long>(psram_kb),
                          static_cast<unsigned long long>(uptime), esp_get_idf_version());
    if (s_ota_status) lv_label_set_text(s_ota_status, ota_service_status());
}

static void battery_animation(lv_timer_t *) {
    float level = 0.0f;
    bool charging = false;
    if (!battery_monitor_read(&level, &charging)) return;
    (void)level;
    if (s_have_charge_state && charging && !s_last_charging) robot_eyes_on_charge_started();
    s_last_charging = charging;
    s_have_charge_state = true;
}

static void create_system_screen() {
    s_system = create_app_screen("System", "Live board information");
    lv_obj_add_event_cb(s_system, gesture_event, LV_EVENT_GESTURE, nullptr);
    lv_obj_t *card = make_card(s_system, 28, 86, 392, 234);
    s_system_values = make_label(card, "Starting...", &lv_font_montserrat_16, COLOR_TEXT);
    lv_obj_set_style_text_align(s_system_values, LV_TEXT_ALIGN_LEFT, 0);
    lv_obj_set_pos(s_system_values, 24, 22);
    lv_obj_set_style_text_line_space(s_system_values, 10, 0);
    lv_timer_create(system_animation, 1000, nullptr);

    lv_obj_t *update = lv_button_create(s_system);
    lv_obj_set_size(update, 214, 34);
    lv_obj_align(update, LV_ALIGN_BOTTOM_MID, 0, -12);
    lv_obj_set_style_bg_color(update, lv_color_hex(COLOR_SURFACE_2), 0);
    lv_obj_set_style_border_color(update, lv_color_hex(COLOR_CYAN), 0);
    lv_obj_set_style_border_width(update, 1, 0);
    lv_obj_set_style_radius(update, LV_RADIUS_CIRCLE, 0);
    lv_obj_add_flag(update, LV_OBJ_FLAG_GESTURE_BUBBLE);
    lv_obj_add_event_cb(update, [](lv_event_t *) { ota_service_start(); },
                        LV_EVENT_SHORT_CLICKED, nullptr);
    lv_obj_t *update_text = make_label(update, "UPDATE FIRMWARE", &lv_font_montserrat_14, COLOR_CYAN);
    lv_obj_center(update_text);

    s_ota_status = make_label(s_system, ota_service_status(), &lv_font_montserrat_14, COLOR_MUTED);
    lv_obj_align(s_ota_status, LV_ALIGN_BOTTOM_MID, 0, -50);
}

static void show_wifi_overview(lv_event_t *) {
    lv_screen_load_anim(s_wifi, LV_SCR_LOAD_ANIM_MOVE_RIGHT, 180, 0, false);
}

static void show_wifi_logs(lv_event_t *) {
    lv_screen_load_anim(s_wifi_logs, LV_SCR_LOAD_ANIM_MOVE_LEFT, 180, 0, false);
}

static void wifi_animation(lv_timer_t *) {
    lv_obj_t *active = lv_screen_active();
    if (active != s_wifi && active != s_wifi_logs) return;

    if (active == s_wifi) {
        WifiServiceInfo wifi = {};
        KageBridgeInfo bridge = {};
        wifi_service_get_info(&wifi);
        kage_bridge_get_info(&bridge);

        if (!wifi.configured) {
            lv_label_set_text(s_wifi_values,
                              "NOT CONFIGURED\n\nUse USB / Improv to add Wi-Fi.");
            lv_obj_set_style_text_color(s_wifi_values, lv_color_hex(COLOR_MUTED), 0);
        } else if (!wifi.connected) {
            lv_label_set_text_fmt(
                s_wifi_values,
                "CONNECTING\nSSID     %s\nLast error reason  %ld\n\nM920q    OFFLINE",
                wifi.ssid, static_cast<long>(wifi.last_disconnect_reason));
            lv_obj_set_style_text_color(s_wifi_values, lv_color_hex(COLOR_TEXT), 0);
        } else {
            lv_label_set_text_fmt(
                s_wifi_values,
                "ONLINE   %d dBm   CH %d\n"
                "SSID     %s\n"
                "IP       %s\n"
                "Gateway  %s\n"
                "BSSID    %s\n"
                "M920q    %s%s\n"
                "Command  %s  #%lu",
                wifi.rssi, wifi.channel, wifi.ssid,
                wifi.ip[0] ? wifi.ip : "-",
                wifi.gateway[0] ? wifi.gateway : "-",
                wifi.bssid[0] ? wifi.bssid : "-",
                bridge.reachable ? "ONLINE" : "OFFLINE",
                bridge.reachable ? "  HTTP 200" : "",
                bridge.command[0] ? bridge.command : "-",
                static_cast<unsigned long>(bridge.sequence));
            lv_obj_set_style_text_color(s_wifi_values, lv_color_hex(COLOR_TEXT), 0);
        }
    }

    if (active == s_wifi_logs) {
        static char log_text[1152];
        event_log_snapshot(log_text, sizeof(log_text));
        lv_label_set_text(s_wifi_logs_values, log_text[0] ? log_text : "No events yet.");
    }
}

static void create_wifi_screen() {
    s_wifi = create_app_screen("Wi-Fi", "Network and M920q assistant bridge");
    lv_obj_add_event_cb(s_wifi, gesture_event, LV_EVENT_GESTURE, nullptr);

    lv_obj_t *card = make_card(s_wifi, 28, 82, 392, 220);
    s_wifi_values = make_label(card, "Starting...", &lv_font_montserrat_14, COLOR_TEXT);
    lv_obj_set_style_text_align(s_wifi_values, LV_TEXT_ALIGN_LEFT, 0);
    lv_obj_set_style_text_line_space(s_wifi_values, 5, 0);
    lv_obj_set_pos(s_wifi_values, 20, 16);
    lv_obj_set_width(s_wifi_values, 350);

    lv_obj_t *logs = lv_button_create(s_wifi);
    lv_obj_set_size(logs, 116, 42);
    lv_obj_align(logs, LV_ALIGN_BOTTOM_RIGHT, -28, -16);
    lv_obj_set_style_bg_color(logs, lv_color_hex(COLOR_SURFACE_2), 0);
    lv_obj_set_style_radius(logs, LV_RADIUS_CIRCLE, 0);
    lv_obj_add_flag(logs, LV_OBJ_FLAG_GESTURE_BUBBLE);
    lv_obj_add_event_cb(logs, show_wifi_logs, LV_EVENT_SHORT_CLICKED, nullptr);
    lv_obj_t *logs_text = make_label(logs, "LOGS", &lv_font_montserrat_14, COLOR_CYAN);
    lv_obj_center(logs_text);

    s_wifi_logs = create_app_screen("Network logs", "Recent Wi-Fi, backend and command events");
    lv_obj_add_event_cb(s_wifi_logs, gesture_event, LV_EVENT_GESTURE, nullptr);
    lv_obj_t *log_card = make_card(s_wifi_logs, 20, 82, 408, 238);
    s_wifi_logs_values = make_label(log_card, "No events yet.", &lv_font_montserrat_14, COLOR_TEXT);
    lv_obj_set_style_text_align(s_wifi_logs_values, LV_TEXT_ALIGN_LEFT, 0);
    lv_obj_set_style_text_line_space(s_wifi_logs_values, 3, 0);
    lv_obj_set_pos(s_wifi_logs_values, 16, 12);
    lv_obj_set_width(s_wifi_logs_values, 374);

    lv_obj_t *back = lv_button_create(s_wifi_logs);
    lv_obj_set_size(back, 86, 36);
    lv_obj_align(back, LV_ALIGN_TOP_RIGHT, -22, 18);
    lv_obj_set_style_bg_color(back, lv_color_hex(COLOR_SURFACE_2), 0);
    lv_obj_set_style_radius(back, LV_RADIUS_CIRCLE, 0);
    lv_obj_add_flag(back, LV_OBJ_FLAG_GESTURE_BUBBLE);
    lv_obj_add_event_cb(back, show_wifi_overview, LV_EVENT_SHORT_CLICKED, nullptr);
    lv_obj_t *back_text = make_label(back, "Wi-Fi", &lv_font_montserrat_14, COLOR_CYAN);
    lv_obj_center(back_text);

    lv_timer_create(wifi_animation, 500, nullptr);
}

static void check_storage(lv_event_t *) {
    if (s_sd_mounted) return;
    lv_label_set_text(s_storage_status, "CHECKING...");
    const esp_err_t result = bsp_sdcard_mount();
    if (result == ESP_OK && bsp_sdcard) {
        s_sd_mounted = true;
        const uint64_t bytes = static_cast<uint64_t>(bsp_sdcard->csd.capacity) *
                               bsp_sdcard->csd.sector_size;
        lv_label_set_text_fmt(s_storage_status, "CARD READY\n%llu MB",
                              static_cast<unsigned long long>(bytes / (1024ULL * 1024ULL)));
        lv_obj_set_style_text_color(s_storage_status, lv_color_hex(COLOR_CYAN), 0);
    } else {
        lv_label_set_text(s_storage_status, "NO CARD FOUND\nInsert a microSD and try again");
        lv_obj_set_style_text_color(s_storage_status, lv_color_hex(COLOR_RED), 0);
    }
}

static void create_storage_screen() {
    s_storage = create_app_screen("Storage", "Check the microSD slot");
    lv_obj_add_event_cb(s_storage, gesture_event, LV_EVENT_GESTURE, nullptr);
    lv_obj_t *button = make_card(s_storage, 96, 112, 256, 128);
    lv_obj_add_flag(button, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(button, check_storage, LV_EVENT_SHORT_CLICKED, nullptr);
    s_storage_status = make_label(button, "TAP TO CHECK", &lv_font_montserrat_16, COLOR_TEXT);
    lv_obj_center(s_storage_status);
}
}  // namespace

void app_shell_begin(lv_display_t *display) {
    s_display = display;
    (void)s_display;
    s_selected = APP_ROBOT;
    create_home_screen();
    create_robot_screen();
    create_microphone_screen();
    create_motion_screen();
    create_display_screen();
    create_system_screen();
    create_wifi_screen();
    create_storage_screen();
    battery_monitor_begin(bsp_i2c_get_handle());
    mic_meter_begin();
    lv_timer_create(battery_animation, 500, nullptr);
    set_app_activity(static_cast<AppId>(-1));
    s_home_last_us = esp_timer_get_time();
    lv_screen_load(s_home);
}
