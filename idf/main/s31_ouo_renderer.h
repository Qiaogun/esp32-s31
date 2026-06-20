#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_lcd_types.h"

#ifdef __cplusplus
extern "C" {
#endif

bool ouo_s31_renderer_begin(esp_lcd_panel_handle_t panel, uint16_t* framebuffer0, uint16_t* framebuffer1, int width, int height);
bool ouo_s31_renderer_ready(void);
void ouo_s31_renderer_tick(uint32_t now_ms);
void ouo_s31_renderer_touch_down(float panel_x, float panel_y);
void ouo_s31_renderer_touch_move(float panel_x, float panel_y);
void ouo_s31_renderer_touch_up(float panel_x, float panel_y);
void ouo_s31_renderer_touch_cancel(void);
bool ouo_s31_renderer_in_menu_trigger_zone(float panel_x, float panel_y);
int ouo_s31_renderer_menu_index_from_panel(float panel_x, float panel_y);
void ouo_s31_renderer_set_menu(bool open, int selected_index);
bool ouo_s31_renderer_set_mood(const char* mood);
void ouo_s31_renderer_set_blush(bool enabled);
bool ouo_s31_renderer_set_option(const char* key, bool enabled);
const char* ouo_s31_renderer_mood_name(void);
float ouo_s31_renderer_idle_time(void);
float ouo_s31_renderer_touch_pressure(void);
int ouo_s31_renderer_mouth_direction(void);
bool ouo_s31_renderer_is_touching(void);
uint32_t ouo_s31_renderer_frame_count(void);
uint32_t ouo_s31_renderer_last_frame_ms(void);

#ifdef __cplusplus
}
#endif
