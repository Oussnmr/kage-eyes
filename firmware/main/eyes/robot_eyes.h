#pragma once

#include "lvgl.h"

enum class RobotEyeState : unsigned char {
    Idle, Happy, Sad, Angry, Sleepy, Surprised, Thinking, Alert, Count
};

void robot_eyes_begin(lv_obj_t *parent);
void robot_eyes_set_state(RobotEyeState state);
RobotEyeState robot_eyes_next_state();
void robot_eyes_set_visible(bool visible);
