#include "typing_scene.hpp"
#include <algorithm>
#include <cmath>

namespace kage_typing {
namespace {
constexpr uint32_t CYAN = 0x4FE3FF;
constexpr uint32_t BLACK = 0x000000;
constexpr uint32_t WHITE = 0xFFFFFF;
constexpr int DEFAULTS[FIELD_COUNT] = {
    196, 369, 143, 112, 112, 28,   // eyes, matching Kage's rounded LVGL boxes
    282, 215, 39, 6,                // mouth, just above keyboard
    10, 245, 428, 110,              // keyboard, white with black key gaps
    92, 251, 69, 186,               // arm root and raised fingertips
    139, 211, 309, 224, 43, 314, 254, 282, 29, // five targets: L/C/R/space/enter
    8, 72, 55, 85, 7000           // hover, squint %, up/down and blink time
};
int limit(int value, int low, int high) { return std::max(low, std::min(high, value)); }

lv_obj_t *plain(lv_obj_t *parent, int x, int y, int w, int h, uint32_t color, int radius = 0) {
    lv_obj_t *o = lv_obj_create(parent);
    lv_obj_remove_style_all(o);
    lv_obj_set_pos(o, x, y);
    lv_obj_set_size(o, w, h);
    lv_obj_set_style_bg_opa(o, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(o, lv_color_hex(color), 0);
    lv_obj_set_style_radius(o, radius, 0);
    lv_obj_remove_flag(o, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(o, LV_OBJ_FLAG_CLICKABLE);
    return o;
}

void line(lv_layer_t *layer, int x1, int y1, int x2, int y2, int width, uint32_t color) {
    lv_draw_line_dsc_t desc;
    lv_draw_line_dsc_init(&desc);
    desc.color = lv_color_hex(color);
    desc.opa = LV_OPA_COVER;
    desc.width = width;
    desc.round_start = 1;
    desc.round_end = 1;
    desc.p1.x = x1;
    desc.p1.y = y1;
    desc.p2.x = x2;
    desc.p2.y = y2;
    lv_draw_line(layer, &desc);
}
}

void Scene::defaults(int *output, int count) const {
    if (!output) return;
    for (int i = 0; i < count && i < FIELD_COUNT; ++i) output[i] = DEFAULTS[i];
}

void Scene::create(lv_obj_t *parent) {
    for (int i = 0; i < FIELD_COUNT; ++i) settings_[i] = DEFAULTS[i];
    screen_ = plain(parent, 0, 0, WIDTH, HEIGHT, BLACK);
    arm_back_ = lv_obj_create(screen_);
    lv_obj_remove_style_all(arm_back_);
    lv_obj_set_size(arm_back_, WIDTH, HEIGHT);
    lv_obj_set_pos(arm_back_, 0, 0);
    lv_obj_add_event_cb(arm_back_, draw_arm_event, LV_EVENT_DRAW_MAIN, this);
    keyboard_ = plain(screen_, 0, 0, WIDTH, HEIGHT, BLACK);
    lv_obj_set_style_bg_opa(keyboard_, LV_OPA_TRANSP, 0);
    arm_front_ = lv_obj_create(screen_);
    lv_obj_remove_style_all(arm_front_);
    lv_obj_set_size(arm_front_, WIDTH, HEIGHT);
    lv_obj_set_pos(arm_front_, 0, 0);
    lv_obj_add_event_cb(arm_front_, draw_arm_event, LV_EVENT_DRAW_MAIN, this);
    eye_left_ = plain(screen_, 0, 0, 1, 1, CYAN, 0);
    eye_right_ = plain(screen_, 0, 0, 1, 1, CYAN, 0);
    mouth_ = plain(screen_, 0, 0, 1, 1, CYAN, 6);
    create_keyboard();
    update_geometry();
    refresh_arms();
}

void Scene::set_mode(bool typing) {
    typing_ = typing;
    if (!typing) timeline_.reset();
    if (keyboard_) {
        if (typing) lv_obj_set_hidden(keyboard_, false);
        else lv_obj_set_hidden(keyboard_, true);
    }
    refresh_arms();
}

void Scene::set_config(const int *config, int count) {
    if (!config || count != FIELD_COUNT) return;
    for (int i = 0; i < FIELD_COUNT; ++i) settings_[i] = config[i];
    settings_[EYE_W] = limit(settings_[EYE_W], 10, 170);
    settings_[EYE_H] = limit(settings_[EYE_H], 10, 180);
    settings_[EYE_RADIUS] = limit(settings_[EYE_RADIUS], 0, 80);
    settings_[KEY_W] = limit(settings_[KEY_W], 140, WIDTH);
    settings_[KEY_H] = limit(settings_[KEY_H], 60, 160);
    settings_[KEY_X] = limit(settings_[KEY_X], 0, WIDTH - settings_[KEY_W]);
    settings_[KEY_Y] = limit(settings_[KEY_Y], 160, HEIGHT - settings_[KEY_H]);
    settings_[ARM_THICKNESS] = limit(settings_[ARM_THICKNESS], 12, 52);
    settings_[HOVER_PX] = limit(settings_[HOVER_PX], 0, 18);
    settings_[SQUINT_PCT] = limit(settings_[SQUINT_PCT], 45, 100);
    settings_[UP_MS] = limit(settings_[UP_MS], 20, 220);
    settings_[DOWN_MS] = limit(settings_[DOWN_MS], 35, 300);
    settings_[BLINK_INTERVAL_MS] = limit(settings_[BLINK_INTERVAL_MS], 1500, 20000);
    create_keyboard();
    update_geometry();
    refresh_arms();
}

void Scene::create_keyboard() {
    if (!keyboard_) return;
    lv_obj_clean(keyboard_);
    const int x = settings_[KEY_X], y = settings_[KEY_Y];
    const int w = settings_[KEY_W], h = settings_[KEY_H];
    plain(keyboard_, x, y, w, h, WHITE, 12);
    const int pad = std::max(5, w / 60);
    plain(keyboard_, x + pad, y + pad, w - pad * 2, h - pad * 2, BLACK, 8);
    const int inset = pad + 5;
    const int interior_w = w - 2 * inset;
    const int interior_h = h - 2 * inset;
    const int rows = 4, gap = std::max(3, h / 29);
    const int key_gap = std::max(3, w / 90);
    const int key_h = (interior_h - (rows - 1) * gap) / rows;
    for (int row = 0; row < rows; ++row) {
        const int yy = y + inset + row * (key_h + gap);
        const int cells = row == 0 ? 8 : (row == 3 ? 9 : 10);
        if (row == 0) {
            const int proportions[] = {1, 1, 5, 1, 1};
            constexpr int total = 9;
            int x0 = x + inset;
            const int available = interior_w - key_gap * 4;
            for (int i = 0; i < 5; ++i) {
                int kw = available * proportions[i] / total;
                if (i == 4) kw = x + inset + interior_w - x0;
                plain(keyboard_, x0, yy, kw, key_h, WHITE, std::min(6, key_h / 3));
                x0 += kw + key_gap;
            }
        } else {
            int x0 = x + inset;
            const int available = interior_w - (cells - 1) * key_gap;
            for (int i = 0; i < cells; ++i) {
                const int end = x + inset + (available * (i + 1)) / cells + i * key_gap;
                const int kw = end - x0;
                plain(keyboard_, x0, yy, kw, key_h, WHITE, std::min(6, key_h / 3));
                x0 = end + key_gap;
            }
        }
    }
    // Version historique : Entrée est un seul bloc blanc à gauche.
    lv_obj_t *enter = lv_obj_create(keyboard_);
    lv_obj_remove_style_all(enter);
    lv_obj_set_pos(enter, x + inset, y + inset + key_h + gap);
    lv_obj_set_size(enter, std::max(8, (interior_w - 9 * key_gap) / 10), key_h * 2 + gap);
    lv_obj_set_style_bg_opa(enter, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(enter, lv_color_hex(WHITE), 0);
    lv_obj_set_style_radius(enter, 5, 0);
}

void Scene::draw_arm_event(lv_event_t *event) {
    Scene *self = static_cast<Scene *>(lv_event_get_user_data(event));
    if (!self || !self->typing_) return;
    const bool raised = lv_event_get_current_target(event) == self->arm_back_;
    const bool is_raised = self->forced_ ? self->forced_frame_ == 0 :
                           self->timeline_.phase() == TypingTimeline::Raised;
    if (raised != is_raised) return;
    self->draw_arm(lv_event_get_layer(event), raised);
}

void Scene::draw_arm(lv_layer_t *layer, bool raised) {
    const int thick = settings_[ARM_THICKNESS];
    const int sx = settings_[ARM_START_X], sy = settings_[ARM_START_Y];
    int tx = settings_[ARM_LIFT_X], ty = settings_[ARM_LIFT_Y];
    if (!raised) {
        const int region = forced_ ? forced_frame_ - 1 : timeline_.region();
        switch (region) {
            case 0: tx = settings_[ARM_LEFT_X]; ty = settings_[ARM_TOUCH_Y]; break;
            case 1: tx = settings_[ARM_CENTER_X]; ty = settings_[ARM_TOUCH_Y]; break;
            case 2: tx = settings_[ARM_RIGHT_X]; ty = settings_[ARM_TOUCH_Y]; break;
            case 3: tx = settings_[ARM_SPACE_X]; ty = settings_[ARM_SPACE_Y]; break;
            default: tx = settings_[ARM_ENTER_X]; ty = settings_[ARM_ENTER_Y]; break;
        }
    }
    const int wrist_x = raised ? tx + thick / 5 : tx - thick / 3;
    const int wrist_y = raised ? ty + thick / 2 : ty - thick / 3;
    // Three blunt fingers from the same wrist. Draw black first to keep a
    // genuine negative-space outline against the white keyboard.
    for (int pass = 0; pass < 2; ++pass) {
        const uint32_t color = pass == 0 ? BLACK : CYAN;
        const int border = pass == 0 ? 7 : 0;
        line(layer, sx, sy, wrist_x, wrist_y, thick + border, color);
        for (int finger = -1; finger <= 1; ++finger) {
            const int spread = thick / 2 + 2;
            const int dx = finger * spread;
            const int fx = raised ? tx + dx : tx + dx + thick / 6;
            const int fy = raised ? ty - (finger == 0 ? 9 : 2) : ty + 8 + (finger == 0 ? 4 : 0);
            line(layer, wrist_x, wrist_y, fx, fy, thick * 2 / 3 + border, color);
        }
    }
}

void Scene::refresh_arms() {
    if (!arm_back_ || !arm_front_) return;
    const bool raised = forced_ ? forced_frame_ == 0 :
                         timeline_.phase() == TypingTimeline::Raised;
    if (typing_ && raised) lv_obj_set_hidden(arm_back_, false);
    else lv_obj_set_hidden(arm_back_, true);
    if (typing_ && !raised) lv_obj_set_hidden(arm_front_, false);
    else lv_obj_set_hidden(arm_front_, true);
    lv_obj_invalidate(arm_back_);
    lv_obj_invalidate(arm_front_);
}

void Scene::update_geometry() {
    if (!eye_left_) return;
    // Idle coordinates are the existing robot_eyes.cpp constants; typing
    // uses an offset, smaller face to leave room for the three-finger arm.
    const int left_x = typing_ ? settings_[EYE_LEFT_X] : 112;
    const int right_x = typing_ ? settings_[EYE_RIGHT_X] : 336;
    const int eye_y = typing_ ? settings_[EYE_Y] : 184;
    const int eye_width = typing_ ? settings_[EYE_W] : 132;
    const int eye_height = typing_ ? settings_[EYE_H] : 132;
    const int radius = typing_ ? settings_[EYE_RADIUS] : 32;
    const int hover_amplitude = typing_ ? settings_[HOVER_PX] : 11;
    const float bob = (std::sin(seconds_ * 2.20f) + std::sin(seconds_ * .65f) * (2.0f / 11.0f)) *
                      hover_amplitude;
    const int offset = static_cast<int>(std::round(bob));
    const bool squint = std::fmod(expression_timer_, 5.5f) > 3.7f &&
                        std::fmod(expression_timer_, 5.5f) < 4.8f;
    const float target = squint ? 1.0f : 0.0f;
    squint_current_ += (target - squint_current_) * 0.19f;
    const float eye_factor = 1.0f -
        squint_current_ * (1.0f - settings_[SQUINT_PCT] / 100.0f);
    const int blink_h = static_cast<int>(eye_height * (1.0f - blink_ * .94f));
    const int height = std::max(4, static_cast<int>(blink_h * eye_factor));
    const int width = eye_width;
    const int y = eye_y + offset - height / 2;
    const int xoffset = static_cast<int>(std::sin(seconds_ * .92f) * 3.0f);
    for (auto entry : {std::pair<lv_obj_t *, int>{eye_left_, left_x},
                       {eye_right_, right_x}}) {
        lv_obj_set_pos(entry.first, entry.second + xoffset - width / 2, y);
        lv_obj_set_size(entry.first, width, height);
        lv_obj_set_style_radius(entry.first, std::min(radius, height / 2), 0);
    }
    const int mouth_w = typing_ ? settings_[MOUTH_W] : 46;
    const int mouth_h = typing_ ? settings_[MOUTH_H] : 7;
    const int mouth_x = typing_ ? settings_[MOUTH_X] : 224;
    const int mouth_y = typing_ ? settings_[MOUTH_Y] : 258;
    lv_obj_set_pos(mouth_, mouth_x - mouth_w / 2, mouth_y + offset);
    lv_obj_set_size(mouth_, mouth_w, mouth_h);
}

void Scene::keypress(int region) {
    forced_ = false;
    if (!typing_) set_mode(true);
    timeline_.press(region);
    refresh_arms();
}

void Scene::set_frame(int frame) {
    forced_ = true;
    forced_frame_ = limit(frame, 0, 3);
    if (!typing_) set_mode(true);
    refresh_arms();
}

void Scene::reset() {
    forced_ = false;
    timeline_.reset();
    seconds_ = expression_timer_ = squint_current_ = blink_ = 0;
    blink_timer_ = 0.3f;
    refresh_arms();
    update_geometry();
}

void Scene::tick(int elapsed_ms) {
    const int dt = limit(elapsed_ms, 0, 100);
    const float ds = dt / 1000.0f;
    seconds_ += ds;
    expression_timer_ += ds;
    blink_timer_ += ds;
    // A short natural blink; the occasional partial squint is independent.
    const float period = settings_[BLINK_INTERVAL_MS] / 1000.0f;
    if (blink_timer_ > period) blink_timer_ -= period;
    if (blink_timer_ < .07f) blink_ = blink_timer_ / .07f;
    else if (blink_timer_ < .23f) blink_ = 1.0f - (blink_timer_ - .07f) / .16f;
    else blink_ = 0;
    const auto previous = timeline_.phase();
    timeline_.tick(dt, settings_[UP_MS], settings_[DOWN_MS]);
    if (!forced_ && previous != timeline_.phase()) refresh_arms();
    update_geometry();
}
} // namespace kage_typing
