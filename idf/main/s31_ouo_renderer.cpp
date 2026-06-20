#include "s31_ouo_renderer.h"

#include <math.h>
#include <string.h>

#include "OuORuntime.h"
#include "esp_cache.h"
#include "esp_lcd_panel_ops.h"
#include "esp_random.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "ouo_assets.h"

namespace {
constexpr uint16_t ColorBlack = 0x0000;
constexpr float DegToRad = 3.14159265358979323846f / 180.0f;
constexpr float MaxRenderScale = 1.0f;

esp_lcd_panel_handle_t s_panel = nullptr;
uint16_t* s_fbs[2] = {};
uint16_t* s_fb = nullptr;
uint8_t s_draw_fb_index = 0;
int s_width = 0;
int s_height = 0;
float s_screen_scale = 1.0f;
float s_origin_x = 0.0f;
float s_origin_y = 0.0f;
int s_render_x0 = 0;
int s_render_y0 = 0;
int s_render_x1 = 0;
int s_render_y1 = 0;
bool s_ready = false;
bool s_menu_open = false;
int s_selected_menu_index = -1;
SemaphoreHandle_t s_lock = nullptr;
OuORuntime s_runtime;
float s_tick_carry_ms = 0.0f;
uint32_t s_last_tick_ms = 0;
uint32_t s_frames = 0;
uint32_t s_last_frame_ms = 0;

uint16_t blend565(uint16_t dst, uint16_t src, uint8_t alpha) {
    uint16_t inv = 255u - alpha;
    uint16_t sr = (src >> 11) & 0x1F;
    uint16_t sg = (src >> 5) & 0x3F;
    uint16_t sb = src & 0x1F;
    uint16_t dr = (dst >> 11) & 0x1F;
    uint16_t dg = (dst >> 5) & 0x3F;
    uint16_t db = dst & 0x1F;
    uint16_t r = (sr * alpha + dr * inv + 127u) / 255u;
    uint16_t g = (sg * alpha + dg * inv + 127u) / 255u;
    uint16_t b = (sb * alpha + db * inv + 127u) / 255u;
    return (uint16_t)((r << 11) | (g << 5) | b);
}

int fast_floor_to_int(float value) {
    int integer = (int)value;
    return value < (float)integer ? integer - 1 : integer;
}

int clamp_int(int value, int min_value, int max_value) {
    if (value < min_value) {
        return min_value;
    }
    if (value > max_value) {
        return max_value;
    }
    return value;
}

void blend_pixel(int x, int y, uint16_t color, uint8_t alpha) {
    if (x < 0 || x >= s_width || y < 0 || y >= s_height || alpha == 0) {
        return;
    }
    uint16_t& dst = s_fb[y * s_width + x];
    dst = alpha == 255 ? color : blend565(dst, color, alpha);
}

void panel_to_runtime_unclamped(float panel_x, float panel_y, float& runtime_x, float& runtime_y) {
    runtime_x = (panel_x - s_origin_x) / s_screen_scale;
    runtime_y = (panel_y - s_origin_y) / s_screen_scale;
}

void draw_sprite(const DrawCommand& command) {
    const OuOSpriteAsset* asset = findOuOSprite(command.imageHandle);
    if (asset == nullptr || command.alpha == 0 || command.scaleX == 0.0f || command.scaleY == 0.0f) {
        return;
    }

    float logical_x = OuORuntime::OriginX + command.x * OuORuntime::DesignScale;
    float logical_y = OuORuntime::OriginY + command.y * OuORuntime::DesignScale;
    float pivot_x = s_origin_x + logical_x * s_screen_scale;
    float pivot_y = s_origin_y + logical_y * s_screen_scale;
    float sx = command.scaleX * s_screen_scale;
    float sy = command.scaleY * s_screen_scale;

    if (fabsf(command.angle) < 0.01f) {
        float left = pivot_x - asset->hotspotX * sx;
        float top = pivot_y - asset->hotspotY * sy;
        float right = left + asset->width * sx;
        float bottom = top + asset->height * sy;
        int x0 = clamp_int((int)floorf(left), 0, s_width - 1);
        int x1 = clamp_int((int)ceilf(right), 0, s_width - 1);
        int y0 = clamp_int((int)floorf(top), 0, s_height - 1);
        int y1 = clamp_int((int)ceilf(bottom), 0, s_height - 1);
        if (x1 < x0 || y1 < y0) {
            return;
        }

        float inv_sx = 1.0f / sx;
        float inv_sy = 1.0f / sy;
        for (int y = y0; y <= y1; ++y) {
            int src_y = (int)(((y + 0.5f) - top) * inv_sy);
            if (src_y < 0 || src_y >= asset->height) {
                continue;
            }
            uint32_t row_offset = asset->offset + (uint32_t)src_y * asset->width;
            for (int x = x0; x <= x1; ++x) {
                int src_x = (int)(((x + 0.5f) - left) * inv_sx);
                if (src_x < 0 || src_x >= asset->width) {
                    continue;
                }
                uint32_t index = row_offset + src_x;
                uint8_t alpha = OUO_SPRITE_ALPHA[index];
                if (alpha == 0) {
                    continue;
                }
                alpha = (uint8_t)(((uint16_t)alpha * command.alpha) / 255u);
                blend_pixel(x, y, OUO_SPRITE_PIXELS[index], alpha);
            }
        }
        return;
    }

    float theta = -command.angle * DegToRad;
    float c = cosf(theta);
    float s = sinf(theta);
    float corners[4][2] = {
        {-asset->hotspotX * sx, -asset->hotspotY * sy},
        {(asset->width - asset->hotspotX) * sx, -asset->hotspotY * sy},
        {-asset->hotspotX * sx, (asset->height - asset->hotspotY) * sy},
        {(asset->width - asset->hotspotX) * sx, (asset->height - asset->hotspotY) * sy},
    };

    float min_x = 9999.0f;
    float max_x = -9999.0f;
    float min_y = 9999.0f;
    float max_y = -9999.0f;
    for (auto& corner : corners) {
        float x = pivot_x + corner[0] * c - corner[1] * s;
        float y = pivot_y + corner[0] * s + corner[1] * c;
        if (x < min_x) min_x = x;
        if (x > max_x) max_x = x;
        if (y < min_y) min_y = y;
        if (y > max_y) max_y = y;
    }

    int x0 = clamp_int((int)floorf(min_x) - 1, 0, s_width - 1);
    int x1 = clamp_int((int)ceilf(max_x) + 1, 0, s_width - 1);
    int y0 = clamp_int((int)floorf(min_y) - 1, 0, s_height - 1);
    int y1 = clamp_int((int)ceilf(max_y) + 1, 0, s_height - 1);
    if (x1 < x0 || y1 < y0) {
        return;
    }

    float inv_sx = 1.0f / sx;
    float inv_sy = 1.0f / sy;
    for (int y = y0; y <= y1; ++y) {
        float dx = (x0 + 0.5f) - pivot_x;
        float dy = (y + 0.5f) - pivot_y;
        float local_x = c * dx + s * dy;
        float local_y = -s * dx + c * dy;
        for (int x = x0; x <= x1; ++x) {
            int src_x = fast_floor_to_int(local_x * inv_sx + asset->hotspotX);
            int src_y = fast_floor_to_int(local_y * inv_sy + asset->hotspotY);
            if (src_x >= 0 && src_x < asset->width && src_y >= 0 && src_y < asset->height) {
                uint32_t index = asset->offset + (uint32_t)src_y * asset->width + src_x;
                uint8_t alpha = OUO_SPRITE_ALPHA[index];
                if (alpha != 0) {
                    alpha = (uint8_t)(((uint16_t)alpha * command.alpha) / 255u);
                    blend_pixel(x, y, OUO_SPRITE_PIXELS[index], alpha);
                }
            }
            local_x += c;
            local_y -= s;
        }
    }
}

void draw_sprite_at_runtime(uint16_t handle, float pivot_x, float pivot_y, float scale, uint8_t alpha) {
    DrawCommand command{
        ObjectKind::MenuButton,
        handle,
        pivot_x / OuORuntime::DesignScale,
        (pivot_y - OuORuntime::OriginY) / OuORuntime::DesignScale,
        scale,
        scale,
        0.0f,
        alpha,
    };
    draw_sprite(command);
}

void fill_circle_runtime(float cx, float cy, float radius, uint16_t color, uint8_t alpha) {
    float panel_cx = s_origin_x + cx * s_screen_scale;
    float panel_cy = s_origin_y + cy * s_screen_scale;
    float panel_radius = radius * s_screen_scale;
    int r = (int)ceilf(panel_radius);
    float r2 = panel_radius * panel_radius;
    int x0 = clamp_int((int)floorf(panel_cx) - r, s_render_x0, s_render_x1 - 1);
    int x1 = clamp_int((int)ceilf(panel_cx) + r, s_render_x0, s_render_x1 - 1);
    int y0 = clamp_int((int)floorf(panel_cy) - r, s_render_y0, s_render_y1 - 1);
    int y1 = clamp_int((int)ceilf(panel_cy) + r, s_render_y0, s_render_y1 - 1);
    for (int y = y0; y <= y1; ++y) {
        float dy = ((float)y + 0.5f) - panel_cy;
        for (int x = x0; x <= x1; ++x) {
            float dx = ((float)x + 0.5f) - panel_cx;
            if (dx * dx + dy * dy <= r2) {
                blend_pixel(x, y, color, alpha);
            }
        }
    }
}

void draw_menu_overlay(void) {
    for (int y = s_render_y0; y < s_render_y1; ++y) {
        for (int x = s_render_x0; x < s_render_x1; ++x) {
            blend_pixel(x, y, ColorBlack, 110);
        }
    }
    fill_circle_runtime(120.0f, 120.0f, 112.0f, 0x7bef, 95);
    fill_circle_runtime(120.0f, 120.0f, 45.0f, ColorBlack, 170);

    for (int i = 0; i < 10; ++i) {
        float angle = (-90.0f + (float)i * 36.0f) * DegToRad;
        float cx = 120.0f + cosf(angle) * 76.0f;
        float cy = 120.0f + sinf(angle) * 76.0f;
        bool selected = s_selected_menu_index == i;
        fill_circle_runtime(cx, cy, selected ? 20.0f : 17.0f, selected ? 0x07ff : 0xc618, selected ? 230 : 220);
        draw_sprite_at_runtime((uint16_t)(1001 + i), cx, cy, selected ? 1.08f : 1.0f, 255);
    }
}

bool render_frame_locked(void) {
    if (s_panel == nullptr || s_fbs[0] == nullptr || s_fbs[1] == nullptr) {
        return false;
    }
    s_fb = s_fbs[s_draw_fb_index];
    for (int y = s_render_y0; y < s_render_y1; ++y) {
        uint16_t* row = s_fb + y * s_width + s_render_x0;
        for (int x = s_render_x0; x < s_render_x1; ++x) {
            *row++ = ColorBlack;
        }
    }

    DrawList list;
    s_runtime.buildDrawList(list);
    for (uint8_t i = 0; i < list.count; ++i) {
        draw_sprite(list.commands[i]);
    }
    if (s_menu_open) {
        draw_menu_overlay();
    }
    if (esp_lcd_panel_draw_bitmap(s_panel, s_render_x0, s_render_y0, s_render_x1, s_render_y1, s_fb) != ESP_OK) {
        return false;
    }
    s_draw_fb_index ^= 1;
    s_frames++;
    return true;
}

bool take_lock(void) {
    return s_lock == nullptr || xSemaphoreTake(s_lock, pdMS_TO_TICKS(100)) == pdTRUE;
}

void give_lock(void) {
    if (s_lock != nullptr) {
        xSemaphoreGive(s_lock);
    }
}

void panel_to_runtime(float panel_x, float panel_y, float& runtime_x, float& runtime_y) {
    panel_to_runtime_unclamped(panel_x, panel_y, runtime_x, runtime_y);
    runtime_x = (float)clamp_int((int)lroundf(runtime_x), 0, (int)OuORuntime::ScreenW);
    runtime_y = (float)clamp_int((int)lroundf(runtime_y), 0, (int)OuORuntime::ScreenH);
}

int menu_index_from_runtime(float runtime_x, float runtime_y) {
    float dx = runtime_x - 120.0f;
    float dy = runtime_y - 120.0f;
    float distance = sqrtf(dx * dx + dy * dy);
    if (distance < 42.0f || distance > 116.0f) {
        return -1;
    }
    float deg = atan2f(dy, dx) * 180.0f / 3.14159265358979323846f;
    float normalized = deg + 90.0f;
    while (normalized < 0.0f) normalized += 360.0f;
    while (normalized >= 360.0f) normalized -= 360.0f;
    return ((int)floorf((normalized + 18.0f) / 36.0f)) % 10;
}
}

extern "C" bool ouo_s31_renderer_begin(esp_lcd_panel_handle_t panel, uint16_t* framebuffer0, uint16_t* framebuffer1, int width, int height) {
    if (panel == nullptr || framebuffer0 == nullptr || framebuffer1 == nullptr || width <= 0 || height <= 0) {
        return false;
    }
    if (s_lock == nullptr) {
        s_lock = xSemaphoreCreateMutex();
        if (s_lock == nullptr) {
            return false;
        }
    }
    if (!take_lock()) {
        return false;
    }
    s_panel = panel;
    s_fbs[0] = framebuffer0;
    s_fbs[1] = framebuffer1;
    s_fb = framebuffer0;
    s_draw_fb_index = 0;
    s_width = width;
    s_height = height;
    float sx = (float)width / OuORuntime::ScreenW;
    float sy = (float)height / OuORuntime::ScreenH;
    s_screen_scale = sx < sy ? sx : sy;
    if (s_screen_scale > MaxRenderScale) {
        s_screen_scale = MaxRenderScale;
    }
    s_origin_x = ((float)width - OuORuntime::ScreenW * s_screen_scale) * 0.5f;
    s_origin_y = ((float)height - OuORuntime::ScreenH * s_screen_scale) * 0.5f;
    s_render_x0 = clamp_int((int)floorf(s_origin_x), 0, width);
    s_render_y0 = clamp_int((int)floorf(s_origin_y), 0, height);
    s_render_x1 = clamp_int((int)ceilf(s_origin_x + OuORuntime::ScreenW * s_screen_scale), 0, width);
    s_render_y1 = clamp_int((int)ceilf(s_origin_y + OuORuntime::ScreenH * s_screen_scale), 0, height);
    memset(s_fbs[0], 0, (size_t)s_width * (size_t)s_height * sizeof(uint16_t));
    memset(s_fbs[1], 0, (size_t)s_width * (size_t)s_height * sizeof(uint16_t));
    esp_cache_msync(s_fbs[0], (size_t)s_width * (size_t)s_height * sizeof(uint16_t), ESP_CACHE_MSYNC_FLAG_DIR_C2M);
    esp_cache_msync(s_fbs[1], (size_t)s_width * (size_t)s_height * sizeof(uint16_t), ESP_CACHE_MSYNC_FLAG_DIR_C2M);
    s_runtime.setRandomSeed(esp_random());
    s_runtime.setMenuHidden(true);
    s_menu_open = false;
    s_selected_menu_index = -1;
    s_ready = true;
    s_tick_carry_ms = 0.0f;
    s_last_tick_ms = 0;
    s_frames = 0;
    s_last_frame_ms = 0;
    if (!render_frame_locked()) {
        s_ready = false;
        give_lock();
        return false;
    }
    give_lock();
    return true;
}

extern "C" bool ouo_s31_renderer_ready(void) {
    return s_ready;
}

extern "C" void ouo_s31_renderer_tick(uint32_t now_ms) {
    if (!s_ready || !take_lock()) {
        return;
    }
    if (s_last_tick_ms == 0) {
        s_last_tick_ms = now_ms;
    }
    uint32_t dt = now_ms - s_last_tick_ms;
    s_last_tick_ms = now_ms;
    s_tick_carry_ms += (float)(dt > 80u ? 80u : dt);
    int tick_guard = 0;
    while (s_tick_carry_ms >= OuORuntime::TickMs && tick_guard < 5) {
        s_runtime.tick();
        s_tick_carry_ms -= OuORuntime::TickMs;
        tick_guard++;
    }
    if (render_frame_locked()) {
        s_last_frame_ms = now_ms;
    }
    give_lock();
}

extern "C" void ouo_s31_renderer_touch_down(float panel_x, float panel_y) {
    if (!s_ready || !take_lock()) {
        return;
    }
    float runtime_x = 0.0f;
    float runtime_y = 0.0f;
    panel_to_runtime(panel_x, panel_y, runtime_x, runtime_y);
    s_runtime.touchDown(runtime_x, runtime_y);
    give_lock();
}

extern "C" void ouo_s31_renderer_touch_move(float panel_x, float panel_y) {
    if (!s_ready || !take_lock()) {
        return;
    }
    float runtime_x = 0.0f;
    float runtime_y = 0.0f;
    panel_to_runtime(panel_x, panel_y, runtime_x, runtime_y);
    s_runtime.touchMove(runtime_x, runtime_y);
    give_lock();
}

extern "C" void ouo_s31_renderer_touch_up(float panel_x, float panel_y) {
    if (!s_ready || !take_lock()) {
        return;
    }
    float runtime_x = 0.0f;
    float runtime_y = 0.0f;
    panel_to_runtime(panel_x, panel_y, runtime_x, runtime_y);
    s_runtime.touchUp(runtime_x, runtime_y);
    give_lock();
}

extern "C" void ouo_s31_renderer_touch_cancel(void) {
    if (!s_ready || !take_lock()) {
        return;
    }
    s_runtime.touchCancel();
    give_lock();
}

extern "C" bool ouo_s31_renderer_in_menu_trigger_zone(float panel_x, float panel_y) {
    if (!s_ready) {
        return false;
    }
    float runtime_x = 0.0f;
    float runtime_y = 0.0f;
    panel_to_runtime_unclamped(panel_x, panel_y, runtime_x, runtime_y);
    return runtime_x >= 76.0f && runtime_x <= 164.0f && runtime_y >= 188.0f && runtime_y <= OuORuntime::ScreenH;
}

extern "C" int ouo_s31_renderer_menu_index_from_panel(float panel_x, float panel_y) {
    if (!s_ready) {
        return -1;
    }
    float runtime_x = 0.0f;
    float runtime_y = 0.0f;
    panel_to_runtime_unclamped(panel_x, panel_y, runtime_x, runtime_y);
    return menu_index_from_runtime(runtime_x, runtime_y);
}

extern "C" void ouo_s31_renderer_set_menu(bool open, int selected_index) {
    if (!s_ready || !take_lock()) {
        return;
    }
    s_menu_open = open;
    s_selected_menu_index = selected_index;
    render_frame_locked();
    give_lock();
}

extern "C" bool ouo_s31_renderer_set_mood(const char* mood) {
    Mood parsed;
    if (!parseMoodName(mood, parsed) || !take_lock()) {
        return false;
    }
    s_runtime.setMood(parsed);
    render_frame_locked();
    give_lock();
    return true;
}

extern "C" void ouo_s31_renderer_set_blush(bool enabled) {
    if (!take_lock()) {
        return;
    }
    s_runtime.setBlush(enabled);
    render_frame_locked();
    give_lock();
}

extern "C" bool ouo_s31_renderer_set_option(const char* key, bool enabled) {
    if (key == nullptr || !take_lock()) {
        return false;
    }
    bool ok = true;
    if (strcmp(key, "lock") == 0) {
        s_runtime.setLockEmotions(enabled);
    } else if (strcmp(key, "static") == 0) {
        s_runtime.setStaticMood(enabled);
    } else if (strcmp(key, "stretch") == 0) {
        s_runtime.setMouthStretchingEnabled(enabled);
    } else if (strcmp(key, "tilt") == 0) {
        s_runtime.setTiltWinkingEnabled(enabled);
    } else {
        ok = false;
    }
    if (ok) {
        render_frame_locked();
    }
    give_lock();
    return ok;
}

extern "C" const char* ouo_s31_renderer_mood_name(void) {
    return moodName(s_runtime.mood());
}

extern "C" float ouo_s31_renderer_idle_time(void) {
    return s_runtime.idleTime();
}

extern "C" float ouo_s31_renderer_touch_pressure(void) {
    return s_runtime.touchPressure();
}

extern "C" int ouo_s31_renderer_mouth_direction(void) {
    return s_runtime.mouthDirection();
}

extern "C" bool ouo_s31_renderer_is_touching(void) {
    return s_runtime.isTouching();
}

extern "C" uint32_t ouo_s31_renderer_frame_count(void) {
    return s_frames;
}

extern "C" uint32_t ouo_s31_renderer_last_frame_ms(void) {
    return s_last_frame_ms;
}
