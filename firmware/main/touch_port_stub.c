#include "touch_port.h"
bool touch_port_init(void){return false;}
void touch_port_poll(void){}
void touch_port_set_tap_callback(touch_tap_callback_t callback,void *context){(void)callback;(void)context;}
