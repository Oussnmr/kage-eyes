#pragma once

#include "lvgl.h"

void robot_eyes_begin(lv_obj_t *parent);

/* Pauses all face work while another application is visible. */
void robot_eyes_set_active(bool active);

/* Thread-safe notification from the IMU task. */
void robot_eyes_on_shake();

/* Thread-safe notification from the read-only battery monitor. */
void robot_eyes_on_charge_started();
