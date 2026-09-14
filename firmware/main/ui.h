#pragma once
#include <stdbool.h>
#include <stdint.h>
void ui_toggle_settings(void);
bool ui_settings_visible(void);
void ui_tap(void);
void ui_hold(void);
void ui_render(uint16_t *frame, int width, int height);
