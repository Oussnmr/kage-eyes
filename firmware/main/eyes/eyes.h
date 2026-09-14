#pragma once
#include <stdint.h>
typedef enum { EYES_NORMAL, EYES_HAPPY, EYES_ANGRY, EYES_SURPRISED, EYES_SLEEPY, EYES_LOOK_LEFT, EYES_LOOK_RIGHT, EYES_EXPRESSION_COUNT } eye_expression_t;
void eyes_init(void);
void eyes_set_expression(eye_expression_t expression);
void eyes_next_expression(void);
void eyes_update(int64_t now_us);
void eyes_render(uint16_t *frame, int width, int height);
eye_expression_t eyes_expression(void);
