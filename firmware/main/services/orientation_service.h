#pragma once

#include "lvgl.h"

/* Starts landscape-only auto-rotation. The face remains 448x368 and flips
   between left-hand and right-hand landscape when the board is tilted. */
bool orientation_service_begin(lv_display_t *display);
