#pragma once
#include "lvgl.h"
#include "typing_timeline.hpp"

namespace kage_typing {
constexpr int WIDTH = 448;
constexpr int HEIGHT = 368;
enum Field {
    EYE_LEFT_X, EYE_RIGHT_X, EYE_Y, EYE_W, EYE_H, EYE_RADIUS,
    MOUTH_X, MOUTH_Y, MOUTH_W, MOUTH_H,
    KEY_X, KEY_Y, KEY_W, KEY_H,
    ARM_START_X, ARM_START_Y, ARM_LIFT_X, ARM_LIFT_Y,
    ARM_LEFT_X, ARM_CENTER_X, ARM_RIGHT_X, ARM_SPACE_X, ARM_ENTER_X,
    ARM_TOUCH_Y, ARM_SPACE_Y, ARM_ENTER_Y, ARM_THICKNESS,
    HOVER_PX, SQUINT_PCT, UP_MS, DOWN_MS, BLINK_INTERVAL_MS,
    FIELD_COUNT
};

class Scene {
public:
    void create(lv_obj_t * parent);
    void set_config(const int *config, int count);
    void defaults(int *output, int count) const;
    void keypress(int region); // 0 left, 1 centre, 2 right, 3 space, 4 enter
    void set_frame(int frame); // 0 raised, 1 left, 2 centre, 3 right
    void reset();
    void tick(int elapsed_ms);
    void set_mode(bool typing);
    bool typing() const { return typing_; }
    const int *config() const { return settings_; }
    const TypingTimeline &timeline() const { return timeline_; }
private:
    static void draw_arm_event(lv_event_t * e);
    void draw_arm(lv_layer_t * layer, bool raised);
    void create_keyboard();
    void update_geometry();
    void refresh_arms();
    lv_obj_t *screen_ = nullptr;
    lv_obj_t *eye_left_ = nullptr;
    lv_obj_t *eye_right_ = nullptr;
    lv_obj_t *mouth_ = nullptr;
    lv_obj_t *arm_back_ = nullptr;
    lv_obj_t *keyboard_ = nullptr;
    lv_obj_t *arm_front_ = nullptr;
    int settings_[FIELD_COUNT] = {};
    TypingTimeline timeline_;
    bool typing_ = true;
    bool forced_ = false;
    int forced_frame_ = 0;
    float seconds_ = 0.0f;
    float expression_timer_ = 0.0f;
    float blink_timer_ = 0.3f;
    float squint_current_ = 0.0f;
    float blink_ = 0.0f;
};
} // namespace kage_typing
