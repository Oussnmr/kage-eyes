/* Port of the user-supplied robot_eyes_blocky.ino to ESP-IDF + LVGL 9.
 * It preserves its SDF eyes, glow, curved lids, breathing, saccades, blink,
 * and interpolated state transitions. */
#include "robot_eyes.h"

#include <cmath>
#include <cstring>

#include "esp_heap_caps.h"
#include "esp_random.h"
#include "esp_timer.h"

namespace {
constexpr int W = 368, H = 448;
constexpr float PI = 3.14159265358979323846f, EYE_CX_OFF = 86, EYE_W = 120, EYE_H = 140, EYE_R = 34, GLOW = 22, BREATHE = 3;
constexpr float AUTO = NAN;
struct Params { float open,w,top,bot,tilt,gaze,gx,gy,blink_min,blink_max,asym; bool flicker; };
static const Params P[] = {
    {1,1,0,0,0,1,AUTO,AUTO,2.2f,5,1,false}, {.58f,1.04f,0,-.5f,0,.4f,AUTO,AUTO,2.5f,5,1,false},
    {.86f,.96f,.1f,0,-10,.5f,AUTO,.55f,3,6,1,false}, {.6f,1,0,0,15,.3f,AUTO,AUTO,2,4,1,false},
    {.28f,1,0,0,3,.3f,AUTO,.75f,1.5f,3,1,false}, {1.28f,.9f,0,0,0,.8f,AUTO,AUTO,3,6,1,false},
    {.82f,1,0,-.18f,4,.2f,-.85f,-.7f,2,4,.55f,false}, {1.1f,1,0,0,8,.6f,AUTO,AUTO,1.1f,2.2f,1,true},
};
static lv_obj_t *canvas;
static uint16_t *pixels;
static RobotEyeState current = RobotEyeState::Idle, previous = RobotEyeState::Idle;
static float mix = 1, time_s = 0, blink = 0, next_blink = 2, gaze_x = 0, gaze_y = 0, target_x = 0, target_y = 0, next_gaze = 1.4f;
static int64_t last_us;
static float clampf(float v, float low, float high) { return v < low ? low : (v > high ? high : v); }
static float lerp(float a, float b, float k) { return a + (b - a) * k; }
static float randomf() { return static_cast<float>(esp_random()) / 4294967295.0f; }
static uint16_t rgb565(uint8_t r, uint8_t g, uint8_t b) { return static_cast<uint16_t>((r & 0xf8) << 8 | (g & 0xfc) << 3 | b >> 3); }
static void max_pixel(int x, int y, uint8_t r, uint8_t g, uint8_t b) {
    if (x < 0 || x >= W || y < 0 || y >= H) return;
    uint16_t old = pixels[y * W + x];
    uint8_t orr = static_cast<uint8_t>((old >> 11) << 3), org = static_cast<uint8_t>(((old >> 5) & 0x3f) << 2), orb = static_cast<uint8_t>((old & 0x1f) << 3);
    pixels[y * W + x] = rgb565(r > orr ? r : orr, g > org ? g : org, b > orb ? b : orb);
}
static float sd_round_rect(float x, float y, float hw, float hh, float radius) {
    float qx = fabsf(x) - (hw - radius), qy = fabsf(y) - (hh - radius), ax = qx > 0 ? qx : 0, ay = qy > 0 ? qy : 0;
    return sqrtf(ax * ax + ay * ay) + fminf(fmaxf(qx, qy), 0) - radius;
}
static void draw_eye(float cx, float cy, const Params &p, float open, int direction, uint8_t cr, uint8_t cg, uint8_t cb, float bright) {
    float hw = EYE_W * p.w * .5f, hh = fmaxf(EYE_H * open * .5f, 3), radius = fminf(EYE_R, fminf(hw - 1, hh - 1));
    float top = p.top * hh * 2, bottom = p.bot * hh * 2, angle = -p.tilt * direction * PI / 180, ca = cosf(angle), sa = sinf(angle), margin = hh + hw + GLOW;
    int x0 = static_cast<int>(clampf(cx - margin, 0, W - 1)), x1 = static_cast<int>(clampf(cx + margin, 0, W - 1));
    int y0 = static_cast<int>(clampf(cy - margin, 0, H - 1)), y1 = static_cast<int>(clampf(cy + margin, 0, H - 1));
    for (int y = y0; y <= y1; ++y) for (int x = x0; x <= x1; ++x) {
        float dx = x - cx, dy = y - cy, lx = dx * ca - dy * sa, ly = dx * sa + dy * ca;
        float n = fminf(1, fabsf(lx) / hw), curve = 1 - n * n, yy = ly < 0 ? ly + top * .5f * curve : ly - bottom * .5f * curve;
        float distance = sd_round_rect(lx, yy, hw, hh, radius);
        if (distance > GLOW) continue;
        float alpha = distance <= 0 ? 1 : powf(1 - distance / GLOW, 2) * .85f;
        alpha *= bright;
        float white = distance <= 0 ? clampf(1 - (yy + hh) / (hh * .7f), 0, 1) * .9f : 0;
        max_pixel(x, y, static_cast<uint8_t>((cr + (255 - cr) * white) * alpha), static_cast<uint8_t>((cg + (255 - cg) * white) * alpha), static_cast<uint8_t>((cb + (255 - cb) * white) * alpha));
    }
}
static void draw_dot(int cx, int cy, int radius, uint8_t r, uint8_t g, uint8_t b, float alpha) {
    for (int y = cy - radius; y <= cy + radius; ++y) for (int x = cx - radius; x <= cx + radius; ++x) {
        float distance = sqrtf(static_cast<float>((x-cx)*(x-cx) + (y-cy)*(y-cy)));
        if (distance <= radius) { float a = alpha * clampf(radius - distance, 0, 1); max_pixel(x, y, static_cast<uint8_t>(r*a), static_cast<uint8_t>(g*a), static_cast<uint8_t>(b*a)); }
    }
}
static void render(lv_timer_t *) {
    int64_t now = esp_timer_get_time(); float dt = static_cast<float>(now - last_us) / 1000000.0f;
    if (dt < .028f) return;
    last_us = now; dt = fminf(dt, .05f); time_s += dt; mix = fminf(1, mix + dt * 4.5f);
    const Params &active = P[static_cast<int>(current)];
    next_blink -= dt; if (blink > 0) blink = fmaxf(0, blink - dt / .13f); else if (next_blink <= 0) { blink = 1; next_blink = lerp(active.blink_min, active.blink_max, randomf()); }
    next_gaze -= dt; if (next_gaze <= 0) { next_gaze = .7f + randomf() * 2.2f; target_x = (randomf()*2-1)*active.gaze; target_y = (randomf()*2-1)*active.gaze*.55f; }
    float follow = fminf(1, dt * 9);
    float goal_x = std::isnan(active.gx) ? target_x : active.gx;
    float goal_y = std::isnan(active.gy) ? target_y : active.gy;
    gaze_x = lerp(gaze_x, goal_x, follow); gaze_y = lerp(gaze_y, goal_y, follow);
    memset(pixels, 0, W * H * sizeof(uint16_t));
    const Params &from = P[static_cast<int>(previous)]; Params p = active;
    p.open=lerp(from.open,active.open,mix); p.w=lerp(from.w,active.w,mix); p.top=lerp(from.top,active.top,mix); p.bot=lerp(from.bot,active.bot,mix); p.tilt=lerp(from.tilt,active.tilt,mix); p.asym=lerp(from.asym,active.asym,mix);
    float blink_factor = 1 - sinf(PI * fminf(1, blink)) * .94f, cy = H*.5f + sinf(time_s*1.1f)*BREATHE + gaze_y*10;
    uint8_t r=0x4f,g=0xe3,b=0xff; if(active.flicker && mix>.5f){r=0xff;g=0x4a;b=0x52;} float brightness=(p.flicker && sinf(time_s*22)>.4f)?.55f:1;
    for(int i=0;i<2;++i){int dir=i?1:-1; float open=p.open*blink_factor; if(p.asym<1 && dir==1)open*=lerp(1,p.asym,mix); draw_eye(W*.5f+dir*EYE_CX_OFF+gaze_x*12,cy,p,open,dir,r,g,b,brightness);}
    if(current==RobotEyeState::Thinking && mix>.4f) for(int i=0;i<3;++i) draw_dot(154+i*30,360,6,r,g,b,.25f+.75f*fmaxf(0,sinf(time_s*3-i*.7f)));
    lv_obj_invalidate(canvas);
}
}

void robot_eyes_begin(lv_obj_t *parent) {
    pixels = static_cast<uint16_t *>(heap_caps_calloc(W * H, sizeof(uint16_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (!pixels) return;
    canvas = lv_canvas_create(parent);
    lv_canvas_set_buffer(canvas, pixels, W, H, LV_COLOR_FORMAT_RGB565);
    lv_obj_set_size(canvas, W, H); lv_obj_center(canvas); last_us = esp_timer_get_time();
    lv_timer_create(render, 16, nullptr);
}
void robot_eyes_set_state(RobotEyeState state) { if (state != current && state < RobotEyeState::Count) { previous=current; current=state; mix=0; } }
RobotEyeState robot_eyes_next_state() { auto next=static_cast<RobotEyeState>((static_cast<int>(current)+1)%static_cast<int>(RobotEyeState::Count)); robot_eyes_set_state(next); return next; }
void robot_eyes_set_visible(bool visible) { if (canvas) { if (visible) lv_obj_clear_flag(canvas, LV_OBJ_FLAG_HIDDEN); else lv_obj_add_flag(canvas, LV_OBJ_FLAG_HIDDEN); } }
