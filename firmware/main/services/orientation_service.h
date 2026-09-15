#pragma once

#include "lvgl.h"

/* Starts landscape-only auto-rotation. The face remains 448x368 and flips
   between left-hand and right-hand landscape when the board is tilted. */
bool orientation_service_begin(lv_display_t *display);

/* Latest accelerometer sample, in m/s². Safe to read from the LVGL task. */
bool orientation_service_get_sample(float *x, float *y, float *z);
bool orientation_service_is_inverted(void);

/* Uses the current resting pose as zero while preserving one g on Z. */
bool orientation_service_calibrate(void);
