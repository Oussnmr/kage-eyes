#pragma once
#include <stdbool.h>
typedef void (*touch_tap_callback_t)(void *context);
bool touch_port_init(void);
void touch_port_poll(void);
void touch_port_set_tap_callback(touch_tap_callback_t callback, void *context);
