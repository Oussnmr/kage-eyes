#include "app_ui.h"

#include <cstdio>

#include "bsp/esp-bsp.h"
#include "robot_eyes.h"
#include "wifi_service.h"

namespace {
constexpr uint32_t CYAN = 0x4fe3ff, CARD = 0x15202b, MUTED = 0x9bb0c1;
static lv_obj_t *home, *settings, *page, *wifi_status;

static void style_page(lv_obj_t *object) {
    lv_obj_set_size(object, 368, 448); lv_obj_set_style_bg_color(object, lv_color_black(), 0);
    lv_obj_set_style_border_width(object, 0, 0); lv_obj_set_style_pad_all(object, 0, 0); lv_obj_clear_flag(object, LV_OBJ_FLAG_SCROLLABLE);
}
static lv_obj_t *label(lv_obj_t *parent, const char *text, int y, const lv_font_t *font, uint32_t color) {
    lv_obj_t *item = lv_label_create(parent); lv_label_set_text(item, text); lv_obj_set_style_text_font(item, font, 0); lv_obj_set_style_text_color(item, lv_color_hex(color), 0); lv_obj_align(item, LV_ALIGN_TOP_MID, 0, y); return item;
}
static void show_home();
static void clear_page() { wifi_status = nullptr; if (page) { lv_obj_delete(page); page = nullptr; } }
static void button_event(lv_event_t *event) { auto callback = reinterpret_cast<void (*)()>(lv_event_get_user_data(event)); callback(); }
static lv_obj_t *button(lv_obj_t *parent, const char *text, int y, void (*action)()) {
    lv_obj_t *item = lv_button_create(parent); lv_obj_set_size(item, 312, 52); lv_obj_align(item, LV_ALIGN_TOP_MID, 0, y);
    lv_obj_set_style_radius(item, 14, 0); lv_obj_set_style_bg_color(item, lv_color_hex(CARD), 0); lv_obj_set_style_border_color(item, lv_color_hex(CYAN), 0); lv_obj_set_style_border_width(item, 1, 0);
    lv_obj_add_event_cb(item, button_event, LV_EVENT_CLICKED, reinterpret_cast<void *>(action));
    lv_obj_t *caption = lv_label_create(item); lv_label_set_text(caption, text); lv_obj_set_style_text_font(caption, &lv_font_montserrat_20, 0); lv_obj_set_style_text_color(caption, lv_color_hex(0xffffff), 0); lv_obj_center(caption); return item;
}
static void back_to_settings();
static void open_display(); static void open_eyes(); static void open_wifi(); static void open_update(); static void open_sd();
static void show_settings() {
    clear_page(); lv_obj_add_flag(home, LV_OBJ_FLAG_HIDDEN); lv_obj_clear_flag(settings, LV_OBJ_FLAG_HIDDEN); robot_eyes_set_visible(false);
}
static void show_home() { clear_page(); lv_obj_add_flag(settings, LV_OBJ_FLAG_HIDDEN); lv_obj_clear_flag(home, LV_OBJ_FLAG_HIDDEN); robot_eyes_set_visible(true); }
static void back_to_settings() { clear_page(); lv_obj_clear_flag(settings, LV_OBJ_FLAG_HIDDEN); }
static void open_display() {
    lv_obj_add_flag(settings, LV_OBJ_FLAG_HIDDEN); page = lv_obj_create(lv_screen_active()); style_page(page); label(page, "Affichage", 34, &lv_font_montserrat_24, CYAN);
    lv_obj_t *value = label(page, "Luminosité", 104, &lv_font_montserrat_20, 0xffffff);
    lv_obj_t *slider = lv_slider_create(page); lv_obj_set_size(slider, 280, 18); lv_obj_align(slider, LV_ALIGN_TOP_MID, 0, 152); lv_slider_set_range(slider, 10, 100); lv_slider_set_value(slider, 80, LV_ANIM_OFF);
    lv_obj_add_event_cb(slider, [](lv_event_t *event) { auto *target = static_cast<lv_obj_t *>(lv_event_get_target(event)); bsp_display_brightness_set(lv_slider_get_value(target)); }, LV_EVENT_VALUE_CHANGED, nullptr);
    (void)value; button(page, "Retour", 326, back_to_settings);
}
static void set_idle(){robot_eyes_set_state(RobotEyeState::Idle);show_home();} static void set_happy(){robot_eyes_set_state(RobotEyeState::Happy);show_home();}
static void set_sad(){robot_eyes_set_state(RobotEyeState::Sad);show_home();} static void set_angry(){robot_eyes_set_state(RobotEyeState::Angry);show_home();}
static void set_sleepy(){robot_eyes_set_state(RobotEyeState::Sleepy);show_home();} static void set_surprised(){robot_eyes_set_state(RobotEyeState::Surprised);show_home();}
static void open_eyes() {
    lv_obj_add_flag(settings, LV_OBJ_FLAG_HIDDEN); page = lv_obj_create(lv_screen_active()); style_page(page); label(page, "Expressions", 22, &lv_font_montserrat_24, CYAN);
    button(page, "Normal", 70, set_idle); button(page, "Heureux", 130, set_happy); button(page, "Triste", 190, set_sad); button(page, "Fâché", 250, set_angry); button(page, "Endormi", 310, set_sleepy); button(page, "Retour", 370, back_to_settings);
}
static void update_wifi_label(lv_timer_t *) { if (wifi_status) { char text[96]; snprintf(text, sizeof(text), "%s\n%s", wifi_service_connected() ? "Connecté à" : "Wi-Fi", wifi_service_name()); lv_label_set_text(wifi_status, text); } }
static void forget_wifi() { wifi_service_forget(); }
static void open_wifi() {
    lv_obj_add_flag(settings, LV_OBJ_FLAG_HIDDEN); page = lv_obj_create(lv_screen_active()); style_page(page); label(page, "Wi-Fi", 30, &lv_font_montserrat_24, CYAN);
    wifi_status = label(page, "Wi-Fi", 94, &lv_font_montserrat_20, 0xffffff);
    label(page, "Pour configurer : branche USB,", 164, &lv_font_montserrat_14, MUTED); label(page, "ouvre l'installateur Kage Eyes", 188, &lv_font_montserrat_14, MUTED); label(page, "dans Chrome ou Edge.", 212, &lv_font_montserrat_14, MUTED);
    button(page, "Oublier ce réseau", 270, forget_wifi); button(page, "Retour", 342, back_to_settings); lv_timer_create(update_wifi_label, 500, nullptr); update_wifi_label(nullptr);
}
static void open_update() { lv_obj_add_flag(settings, LV_OBJ_FLAG_HIDDEN); page = lv_obj_create(lv_screen_active()); style_page(page); label(page, "Mise à jour", 38, &lv_font_montserrat_24, CYAN); label(page, "La mise à jour OTA arrive", 132, &lv_font_montserrat_20, 0xffffff); label(page, "après validation du Wi-Fi.", 162, &lv_font_montserrat_14, MUTED); button(page, "Retour", 330, back_to_settings); }
static void open_sd() { lv_obj_add_flag(settings, LV_OBJ_FLAG_HIDDEN); page = lv_obj_create(lv_screen_active()); style_page(page); label(page, "Carte SD", 38, &lv_font_montserrat_24, CYAN); label(page, "Lecture des fichiers :", 132, &lv_font_montserrat_20, 0xffffff); label(page, "prévue pour la phase audio.", 162, &lv_font_montserrat_14, MUTED); button(page, "Retour", 330, back_to_settings); }
static void next_eye() { robot_eyes_next_state(); }
}

void app_ui_begin(void) {
    lv_obj_t *screen = lv_screen_active(); lv_obj_set_style_bg_color(screen, lv_color_black(), 0); lv_obj_clear_flag(screen, LV_OBJ_FLAG_SCROLLABLE);
    home = lv_obj_create(screen); style_page(home); robot_eyes_begin(home);
    lv_obj_t *tap = lv_button_create(home); lv_obj_set_size(tap, 368, 448); lv_obj_set_style_bg_opa(tap, LV_OPA_TRANSP, 0); lv_obj_set_style_border_width(tap, 0, 0); lv_obj_add_event_cb(tap, [](lv_event_t *) { next_eye(); }, LV_EVENT_CLICKED, nullptr);
    lv_obj_t *menu = lv_button_create(home); lv_obj_set_size(menu, 112, 44); lv_obj_align(menu, LV_ALIGN_TOP_RIGHT, -16, 16); lv_obj_set_style_bg_color(menu, lv_color_hex(0x06141c), 0); lv_obj_set_style_border_color(menu, lv_color_hex(CYAN), 0); lv_obj_set_style_border_width(menu, 1, 0); lv_obj_add_event_cb(menu, [](lv_event_t *) { show_settings(); }, LV_EVENT_CLICKED, nullptr); lv_obj_t *menu_label = lv_label_create(menu); lv_label_set_text(menu_label, "MENU"); lv_obj_set_style_text_font(menu_label, &lv_font_montserrat_16, 0); lv_obj_center(menu_label);
    settings = lv_obj_create(screen); style_page(settings); lv_obj_add_flag(settings, LV_OBJ_FLAG_HIDDEN); label(settings, "Réglages", 18, &lv_font_montserrat_24, CYAN);
    button(settings, "Wi-Fi", 68, open_wifi); button(settings, "Affichage", 124, open_display); button(settings, "Yeux", 180, open_eyes); button(settings, "Carte SD", 236, open_sd); button(settings, "Mise à jour", 292, open_update); button(settings, "Retour", 348, show_home);
}
