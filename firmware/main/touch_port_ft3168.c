/* One portable tap event for both Waveshare board revisions. */
#include "touch_port.h"
#include "board_pins.h"
#include "esp_lcd_touch_ft5x06.h"
#include "esp_lcd_touch_cst816s.h"
#include "esp_lcd_panel_io.h"
#include "esp_timer.h"
#include "esp_log.h"
#include "driver/i2c_master.h"
static esp_lcd_touch_handle_t s_touch; static bool s_down; static int64_t s_pressed;
static bool s_hold_sent;
static touch_tap_callback_t s_callback; static void *s_context;
static touch_hold_callback_t s_hold_callback; static void *s_hold_context;
extern i2c_master_bus_handle_t board_i2c_bus(void); extern bool board_is_v2(void);
bool touch_port_init(void){const bool v2=board_is_v2();esp_lcd_panel_io_handle_t io;esp_lcd_panel_io_i2c_config_t cfg=v2?(esp_lcd_panel_io_i2c_config_t)ESP_LCD_TOUCH_IO_I2C_CST816S_CONFIG():(esp_lcd_panel_io_i2c_config_t)ESP_LCD_TOUCH_IO_I2C_FT5x06_CONFIG();cfg.dev_addr=v2?I2C_ADDR_CST816:I2C_ADDR_FT3168;cfg.scl_speed_hz=400000;if(esp_lcd_new_panel_io_i2c(board_i2c_bus(),&cfg,&io)!=ESP_OK)return false;esp_lcd_touch_config_t tc={.x_max=PANEL_W,.y_max=PANEL_H,.rst_gpio_num=-1,.int_gpio_num=-1};esp_err_t err=v2?esp_lcd_touch_new_i2c_cst816s(io,&tc,&s_touch):esp_lcd_touch_new_i2c_ft5x06(io,&tc,&s_touch);ESP_LOGI("touch","%s %s",v2?"CST816":"FT3168",err==ESP_OK?"ready":"unavailable");return err==ESP_OK;}
void touch_port_set_tap_callback(touch_tap_callback_t callback,void *context){s_callback=callback;s_context=context;}
void touch_port_set_hold_callback(touch_hold_callback_t callback,void *context){s_hold_callback=callback;s_hold_context=context;}
void touch_port_poll(void){if(!s_touch)return;uint16_t x,y,strength;uint8_t n=0;esp_lcd_touch_read_data(s_touch);bool down=esp_lcd_touch_get_coordinates(s_touch,&x,&y,&strength,&n,1)&&n>0;int64_t now=esp_timer_get_time();if(down&&!s_down){s_pressed=now;s_hold_sent=false;}if(down&&!s_hold_sent&&now-s_pressed>=650000&&s_hold_callback){s_hold_callback(s_hold_context);s_hold_sent=true;}if(!down&&s_down&&!s_hold_sent&&now-s_pressed<350000&&s_callback)s_callback(s_context);s_down=down;}
