#pragma once

#include "lvgl.h"

void robot_eyes_begin(lv_obj_t *parent);

/* Thread-safe notification from the IMU task. */
void robot_eyes_on_shake();
