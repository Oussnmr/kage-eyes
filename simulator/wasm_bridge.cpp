#include "typing_scene.hpp"
#include <algorithm>
#include <cstdint>
#include <cstring>
#ifdef __EMSCRIPTEN__
#include <emscripten/emscripten.h>
#else
#define EMSCRIPTEN_KEEPALIVE
#endif

namespace {
alignas(16) uint8_t draw_buffer[kage_typing::WIDTH * 44 * 2];
alignas(16) uint16_t pixels[kage_typing::WIDTH * kage_typing::HEIGHT];
kage_typing::Scene scene;
bool initialized = false;

void flush(lv_display_t *display, const lv_area_t *area, uint8_t *src) {
    const int span = area->x2 - area->x1 + 1;
    for (int y = area->y1; y <= area->y2; ++y) {
        if (y < 0 || y >= kage_typing::HEIGHT) continue;
        std::memcpy(pixels + y * kage_typing::WIDTH + area->x1,
                    src + (y - area->y1) * span * 2, span * 2);
    }
    lv_display_flush_ready(display);
}
}

extern "C" {
EMSCRIPTEN_KEEPALIVE int kage_init() {
    if (initialized) return 1;
    lv_init();
    lv_display_t *display = lv_display_create(kage_typing::WIDTH, kage_typing::HEIGHT);
    if (!display) return 0;
    lv_display_set_color_format(display, LV_COLOR_FORMAT_RGB565);
    lv_display_set_buffers(display, draw_buffer, nullptr, sizeof(draw_buffer),
                           LV_DISPLAY_RENDER_MODE_PARTIAL);
    lv_display_set_flush_cb(display, flush);
    lv_obj_t *root = lv_screen_active();
    lv_obj_remove_style_all(root);
    lv_obj_set_style_bg_color(root, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(root, LV_OPA_COVER, 0);
    scene.create(root);
    initialized = true;
    lv_timer_handler();
    return 1;
}
EMSCRIPTEN_KEEPALIVE uintptr_t kage_framebuffer() { return reinterpret_cast<uintptr_t>(pixels); }
EMSCRIPTEN_KEEPALIVE int kage_field_count() { return kage_typing::FIELD_COUNT; }
EMSCRIPTEN_KEEPALIVE void kage_defaults(int *out, int count) { scene.defaults(out, count); }
EMSCRIPTEN_KEEPALIVE void kage_set_config(const int *in, int count) {
    if (initialized) scene.set_config(in, count);
}
EMSCRIPTEN_KEEPALIVE void kage_press(int region) { if (initialized) scene.keypress(region); }
EMSCRIPTEN_KEEPALIVE void kage_frame(int frame) { if (initialized) scene.set_frame(frame); }
EMSCRIPTEN_KEEPALIVE void kage_mode(int typing) { if (initialized) scene.set_mode(typing != 0); }
EMSCRIPTEN_KEEPALIVE void kage_reset() { if (initialized) scene.reset(); }
EMSCRIPTEN_KEEPALIVE unsigned kage_strokes() { return scene.timeline().strokes(); }
EMSCRIPTEN_KEEPALIVE int kage_pending() { return scene.timeline().pending(); }
EMSCRIPTEN_KEEPALIVE int kage_is_raised() {
    return scene.timeline().phase() == TypingTimeline::Raised;
}
EMSCRIPTEN_KEEPALIVE void kage_tick(int delta_ms) {
    if (!initialized) return;
    const int dt = std::clamp(delta_ms, 0, 100);
    lv_tick_inc(dt);
    scene.tick(dt);
    lv_timer_handler();
}
}
