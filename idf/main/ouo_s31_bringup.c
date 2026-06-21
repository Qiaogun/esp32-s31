#include <ctype.h>
#include <inttypes.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "driver/ledc.h"
#include "driver/rmt_tx.h"
#include "driver/uart.h"
#include "esp_cache.h"
#include "esp_cam_ctlr.h"
#include "esp_cam_ctlr_dvp.h"
#include "esp_cam_sensor.h"
#include "esp_cam_sensor_detect.h"
#include "esp_chip_info.h"
#include "esp_event.h"
#include "esp_flash.h"
#include "esp_heap_caps.h"
#include "esp_https_ota.h"
#include "esp_http_client.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_panel_rgb.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "esp_ota_ops.h"
#include "esp_partition.h"
#include "esp_spiffs.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "esp_sccb_i2c.h"
#include "esp_sccb_intf.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "led_strip_encoder.h"
#include "mbedtls/base64.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "psa/crypto.h"
#include "s31_ouo_renderer.h"

#define OUO_SCAN_MAX_APS 12
#define OUO_FUNCTION_RGB_LED_GPIO 60
#define OUO_KORVO_RGB_LED_GPIO 37
#define OUO_RMT_LED_STRIP_RESOLUTION_HZ 10000000
#define OUO_KORVO_LCD_H_RES 800
#define OUO_KORVO_LCD_V_RES 480
#define OUO_KORVO_LCD_PCLK_HZ (18 * 1000 * 1000)
#define OUO_KORVO_TOUCH_SDA_GPIO 0
#define OUO_KORVO_TOUCH_SCL_GPIO 1
#define OUO_GT1151_I2C_ADDR 0x14
#define OUO_GT1151_STATUS_REG 0x814e
#define OUO_GT1151_POINT_REG 0x814f
#define OUO_TOUCH_STALE_CANCEL_MS 1500
#define OUO_MENU_LONG_PRESS_MS 700
#define OUO_KORVO_CAMERA_RESET_GPIO 60
#define OUO_KORVO_CAMERA_PWDN_GPIO 61
#define OUO_KORVO_CAMERA_XCLK_GPIO 55
#define OUO_KORVO_CAMERA_PCLK_GPIO 54
#define OUO_KORVO_CAMERA_DE_GPIO 57
#define OUO_KORVO_CAMERA_VSYNC_GPIO 56
#define OUO_KORVO_CAMERA_D0_GPIO 46
#define OUO_KORVO_CAMERA_D1_GPIO 47
#define OUO_KORVO_CAMERA_D2_GPIO 48
#define OUO_KORVO_CAMERA_D3_GPIO 49
#define OUO_KORVO_CAMERA_D4_GPIO 50
#define OUO_KORVO_CAMERA_D5_GPIO 51
#define OUO_KORVO_CAMERA_D6_GPIO 52
#define OUO_KORVO_CAMERA_D7_GPIO 53
#define OUO_KORVO_CAMERA_XCLK_HZ 20000000
#define OUO_KORVO_CAMERA_DATA_WIDTH CAM_CTLR_DATA_WIDTH_8
#define OUO_CAMERA_DEFAULT_PREVIEW_SECONDS 15
#define OUO_CAMERA_MAX_PREVIEW_SECONDS 120
#define OUO_CAMERA_RGB565_BYTES_PER_PIXEL 2
#define OUO_CAMERA_SCCB_FREQ_HZ 100000
#define OUO_FIRMWARE_VERSION "0.2.0-ai-home-link"
#define OUO_AI_HTTP_TIMEOUT_MS 8000
#define OUO_AI_CAMERA_HTTP_TIMEOUT_MS 20000
#define OUO_OTA_HTTP_TIMEOUT_MS 60000
#define OUO_AI_HTTP_RESPONSE_MAX 2048
#define OUO_AI_HEARTBEAT_INTERVAL_MS 30000

typedef struct {
    const char* board;
    const char* mood;
    bool blush;
    bool lock;
    bool static_mood;
    bool stretch;
    bool tilt;
    bool lcd_ready;
    bool touch_ready;
    bool touch_active;
    bool renderer_ready;
    const char* display_mode;
    uint16_t touch_x;
    uint16_t touch_y;
    uint32_t touch_events;
    uint32_t touch_errors;
    uint32_t boot_ms;
    uint32_t boot_count;
    uint32_t commands;
} ouo_state_t;

typedef struct {
    const char* name;
    int width;
    int height;
} ouo_camera_preview_format_t;

typedef struct {
    esp_cam_sensor_device_t* sensor;
    esp_sccb_io_handle_t sccb_handle;
} ouo_camera_sensor_session_t;

typedef struct {
    char* data;
    int len;
    int cap;
} ouo_http_response_t;

typedef struct {
    psa_hash_operation_t* op;
    uint32_t bytes;
    esp_err_t err;
} ouo_sha256_download_t;

typedef struct {
    char version[32];
    char channel[16];
    char firmware_url[192];
    char sha256[80];
    uint32_t size;
} ouo_ota_manifest_t;

static const char* TAG = "ouo_s31";
static bool s_wifi_ready = false;
static bool s_storage_ready = false;
static esp_netif_t* s_sta_netif = NULL;
static rmt_channel_handle_t s_led_channel = NULL;
static rmt_encoder_handle_t s_led_encoder = NULL;
static int s_led_gpio = -1;
static esp_lcd_panel_handle_t s_lcd_panel = NULL;
static i2c_master_bus_handle_t s_i2c_bus = NULL;
static i2c_master_dev_handle_t s_touch_dev = NULL;
static TaskHandle_t s_render_task_handle = NULL;
static bool s_touch_last = false;
static bool s_touch_forwarded = false;
static bool s_menu_candidate = false;
static bool s_menu_open = false;
static int s_selected_menu_index = -1;
static uint32_t s_touch_down_ms = 0;
static bool s_camera_xclk_ready = false;
static bool s_camera_preview_active = false;
static volatile uint32_t s_camera_preview_new_trans = 0;
static volatile uint32_t s_camera_preview_finished_trans = 0;
static char s_ai_server_url[128] = "http://127.0.0.1:8787";
static char s_ota_manifest_url[160] = "http://127.0.0.1:8787/api/v1/ota/manifest";
static char s_wifi_ssid[33] = "";
static char s_wifi_password[65] = "";
static char s_last_ota_version[32] = "";
static char s_last_ota_status[32] = "idle";
static char s_last_wake_phrase[64] = "";
static char s_current_mood[24] = "smile";
static uint8_t s_last_wake_confidence = 0;
static bool s_wifi_autoconnect = true;
static bool s_ai_home_autostart = false;
static TaskHandle_t s_ai_home_task_handle = NULL;
static esp_err_t ensure_korvo_lcd_ready(void);
static ouo_state_t s_state = {
    .board = "ESP32-S31-Korvo-1",
    .mood = s_current_mood,
    .display_mode = "booting",
    .stretch = true,
    .tilt = true,
};

static void trim_line(char* text) {
    size_t len = strlen(text);
    while (len > 0 && isspace((unsigned char)text[len - 1])) {
        text[--len] = '\0';
    }
    char* start = text;
    while (*start != '\0' && isspace((unsigned char)*start)) {
        start++;
    }
    if (start != text) {
        memmove(text, start, strlen(start) + 1);
    }
}

static void lower_line(char* text) {
    for (; *text != '\0'; ++text) {
        *text = (char)tolower((unsigned char)*text);
    }
}

static bool parse_on_off(const char* value, bool* out) {
    if (strcmp(value, "on") == 0 || strcmp(value, "1") == 0 || strcmp(value, "true") == 0 || strcmp(value, "yes") == 0) {
        *out = true;
        return true;
    }
    if (strcmp(value, "off") == 0 || strcmp(value, "0") == 0 || strcmp(value, "false") == 0 || strcmp(value, "no") == 0) {
        *out = false;
        return true;
    }
    return false;
}

static void ai_home_post_wake_event(const char* phrase, float confidence);
static void ai_home_camera_snapshot_command(void);

static bool valid_mood(const char* mood) {
    static const char* moods[] = {
        "smile", "grump", "angry", "surprise", "blink", "squint",
        "sad", "blank", "upset", "blink2", "cheeky", "frown",
    };
    for (size_t i = 0; i < sizeof(moods) / sizeof(moods[0]); ++i) {
        if (strcmp(mood, moods[i]) == 0) {
            return true;
        }
    }
    return false;
}

static const char* menu_mood_name(int index) {
    static const char* moods[] = {
        "smile", "grump", "surprise", "squint", "sad",
        "blank", "upset", "blink", "cheeky", "frown",
    };
    if (index < 0 || index >= (int)(sizeof(moods) / sizeof(moods[0]))) {
        return "smile";
    }
    return moods[index];
}

static const char* canonical_mood_name(const char* mood) {
    return strcmp(mood, "angry") == 0 ? "grump" : mood;
}

static bool set_current_mood(const char* mood) {
    if (mood == NULL || !valid_mood(mood)) {
        return false;
    }
    const char* canonical = canonical_mood_name(mood);
    snprintf(s_current_mood, sizeof(s_current_mood), "%s", canonical);
    s_state.mood = s_current_mood;
    if (s_state.renderer_ready && !ouo_s31_renderer_set_mood(s_current_mood)) {
        return false;
    }
    return true;
}

static void update_ota_manifest_url(void) {
    snprintf(s_ota_manifest_url, sizeof(s_ota_manifest_url), "%s/api/v1/ota/manifest", s_ai_server_url);
}

static void save_ai_home_config(void) {
    nvs_handle_t nvs = 0;
    esp_err_t err = nvs_open("ouo", NVS_READWRITE, &nvs);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "nvs_open ai_home failed: %s", esp_err_to_name(err));
        return;
    }
    err = nvs_set_str(nvs, "ai_server", s_ai_server_url);
    if (err == ESP_OK) {
        err = nvs_set_u8(nvs, "ai_autostart", s_ai_home_autostart ? 1 : 0);
    }
    if (err == ESP_OK) {
        err = nvs_set_str(nvs, "ota_manifest", s_ota_manifest_url);
    }
    if (err == ESP_OK) {
        err = nvs_commit(nvs);
    }
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "nvs ai_home commit failed: %s", esp_err_to_name(err));
    }
    nvs_close(nvs);
}

static void load_ai_home_config(void) {
    nvs_handle_t nvs = 0;
    esp_err_t err = nvs_open("ouo", NVS_READONLY, &nvs);
    if (err != ESP_OK) {
        update_ota_manifest_url();
        return;
    }

    size_t len = sizeof(s_ai_server_url);
    err = nvs_get_str(nvs, "ai_server", s_ai_server_url, &len);
    if (err != ESP_OK && err != ESP_ERR_NVS_NOT_FOUND) {
        ESP_LOGW(TAG, "nvs_get_str ai_server failed: %s", esp_err_to_name(err));
    }

    uint8_t autostart = 0;
    err = nvs_get_u8(nvs, "ai_autostart", &autostart);
    if (err == ESP_OK) {
        s_ai_home_autostart = autostart != 0;
    } else if (err != ESP_ERR_NVS_NOT_FOUND) {
        ESP_LOGW(TAG, "nvs_get_u8 ai_autostart failed: %s", esp_err_to_name(err));
    }

    update_ota_manifest_url();
    len = sizeof(s_ota_manifest_url);
    err = nvs_get_str(nvs, "ota_manifest", s_ota_manifest_url, &len);
    if (err != ESP_OK && err != ESP_ERR_NVS_NOT_FOUND) {
        ESP_LOGW(TAG, "nvs_get_str ota_manifest failed: %s", esp_err_to_name(err));
    }

    nvs_close(nvs);
}

static void save_wifi_config(void) {
    nvs_handle_t nvs = 0;
    esp_err_t err = nvs_open("ouo", NVS_READWRITE, &nvs);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "nvs_open wifi failed: %s", esp_err_to_name(err));
        return;
    }
    err = nvs_set_u8(nvs, "wifi_auto", s_wifi_autoconnect ? 1 : 0);
    if (err == ESP_OK) {
        err = nvs_set_str(nvs, "wifi_ssid", s_wifi_ssid);
    }
    if (err == ESP_OK) {
        err = nvs_set_str(nvs, "wifi_pass", s_wifi_password);
    }
    if (err == ESP_OK) {
        err = nvs_commit(nvs);
    }
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "nvs wifi commit failed: %s", esp_err_to_name(err));
    }
    nvs_close(nvs);
}

static void load_wifi_config(void) {
    nvs_handle_t nvs = 0;
    esp_err_t err = nvs_open("ouo", NVS_READONLY, &nvs);
    if (err != ESP_OK) {
        return;
    }

    uint8_t autoconnect = 1;
    err = nvs_get_u8(nvs, "wifi_auto", &autoconnect);
    if (err == ESP_OK) {
        s_wifi_autoconnect = autoconnect != 0;
    } else if (err != ESP_ERR_NVS_NOT_FOUND) {
        ESP_LOGW(TAG, "nvs_get_u8 wifi_auto failed: %s", esp_err_to_name(err));
    }

    size_t len = sizeof(s_wifi_ssid);
    err = nvs_get_str(nvs, "wifi_ssid", s_wifi_ssid, &len);
    if (err != ESP_OK && err != ESP_ERR_NVS_NOT_FOUND) {
        ESP_LOGW(TAG, "nvs_get_str wifi_ssid failed: %s", esp_err_to_name(err));
    }

    len = sizeof(s_wifi_password);
    err = nvs_get_str(nvs, "wifi_pass", s_wifi_password, &len);
    if (err != ESP_OK && err != ESP_ERR_NVS_NOT_FOUND) {
        ESP_LOGW(TAG, "nvs_get_str wifi_pass failed: %s", esp_err_to_name(err));
    }

    nvs_close(nvs);
}

static void clear_wifi_config(void) {
    s_wifi_ssid[0] = '\0';
    s_wifi_password[0] = '\0';
    s_wifi_autoconnect = false;

    nvs_handle_t nvs = 0;
    esp_err_t err = nvs_open("ouo", NVS_READWRITE, &nvs);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "nvs_open wifi clear failed: %s", esp_err_to_name(err));
        return;
    }
    esp_err_t erase_ssid = nvs_erase_key(nvs, "wifi_ssid");
    esp_err_t erase_pass = nvs_erase_key(nvs, "wifi_pass");
    err = nvs_set_u8(nvs, "wifi_auto", 0);
    if (err == ESP_OK) {
        err = nvs_commit(nvs);
    }
    if (err != ESP_OK ||
        (erase_ssid != ESP_OK && erase_ssid != ESP_ERR_NVS_NOT_FOUND) ||
        (erase_pass != ESP_OK && erase_pass != ESP_ERR_NVS_NOT_FOUND)) {
        ESP_LOGW(TAG,
                 "nvs wifi clear partial err=%s ssid=%s pass=%s",
                 esp_err_to_name(err),
                 esp_err_to_name(erase_ssid),
                 esp_err_to_name(erase_pass));
    }
    nvs_close(nvs);
}

static void print_help(void) {
    printf("commands:\n");
    printf("  help\n");
    printf("  status | diag\n");
    printf("  mac\n");
    printf("  partitions\n");
    printf("  storage_test\n");
    printf("  wifi_scan\n");
    printf("  wifi_status\n");
    printf("  wifi_connect <ssid> <password>\n");
    printf("  wifi_autoconnect on|off\n");
    printf("  wifi_forget\n");
    printf("  ai_home_status\n");
    printf("  ai_home_server <url>\n");
    printf("  ai_home_ping\n");
    printf("  ai_home_poll\n");
    printf("  ai_home_dialog <text>\n");
    printf("  ai_home_camera_snapshot\n");
    printf("  ai_home_autostart on|off\n");
    printf("  wake <phrase> [0.0-1.0]\n");
    printf("  emotion_map <text>\n");
    printf("  ota_status\n");
    printf("  ota_check\n");
    printf("  ota_update\n");
    printf("  ota_manifest_url <url>\n");
    printf("  board_info\n");
    printf("  lcd_probe\n");
    printf("  lcd_test\n");
    printf("  renderer_test\n");
    printf("  touch_status\n");
    printf("  camera_probe\n");
    printf("  camera_preview [seconds]\n");
    printf("  camera_focus\n");
    printf("  korvo_led_test [gpio]\n");
    printf("  function_led_test\n");
    printf("  mood smile|grump|angry|surprise|blink|squint|sad|blank|upset|blink2|cheeky|frown\n");
    printf("  blush on|off\n");
    printf("  option lock|static|stretch|tilt on|off\n");
    printf("  reboot\n");
}

static const char* emotion_for_text(const char* text, uint8_t* confidence) {
    if (strstr(text, "惊喜") != NULL || strstr(text, "厉害") != NULL || strstr(text, "wow") != NULL) {
        *confidence = 86;
        return "surprise";
    }
    if (strstr(text, "难过") != NULL || strstr(text, "伤心") != NULL || strstr(text, "sad") != NULL) {
        *confidence = 82;
        return "sad";
    }
    if (strstr(text, "生气") != NULL || strstr(text, "烦") != NULL || strstr(text, "angry") != NULL || strstr(text, "坏了") != NULL) {
        *confidence = 80;
        return "grump";
    }
    if (strstr(text, "调皮") != NULL || strstr(text, "玩笑") != NULL || strstr(text, "joke") != NULL) {
        *confidence = 78;
        return "cheeky";
    }
    if (strstr(text, "困") != NULL || strstr(text, "累") != NULL || strstr(text, "sleepy") != NULL) {
        *confidence = 74;
        return "squint";
    }
    if (strstr(text, "担心") != NULL || strstr(text, "糟糕") != NULL || strstr(text, "worried") != NULL) {
        *confidence = 74;
        return "upset";
    }
    *confidence = 58;
    return "smile";
}

static void apply_mood_from_ai(const char* mood) {
    (void)set_current_mood(mood);
}

static void ai_home_status_command(void) {
    printf("ai_home firmware=%s server_url=\"%s\" ota_manifest=\"%s\"\n",
           OUO_FIRMWARE_VERSION,
           s_ai_server_url,
           s_ota_manifest_url);
    printf("ai_home wake_phrase=\"%s\" wake_confidence=%u mood=%s camera_preview=%d server_audio=required\n",
           s_last_wake_phrase,
           s_last_wake_confidence,
           s_state.mood,
           s_camera_preview_active);
    printf("ai_home autostart=%d heartbeat_interval_ms=%d\n",
           s_ai_home_autostart,
           OUO_AI_HEARTBEAT_INTERVAL_MS);
    printf("ai_home api heartbeat=POST /api/v1/device/heartbeat command=POST /api/v1/device/command wake=POST /api/v1/wake dialog=POST /api/v1/dialog camera=POST /api/v1/camera/frame\n");
}

static void ai_home_server_command(const char* args) {
    char url[sizeof(s_ai_server_url)] = {};
    snprintf(url, sizeof(url), "%s", args == NULL ? "" : args);
    trim_line(url);
    if (url[0] == '\0') {
        printf("err use: ai_home_server <url>\n");
        return;
    }
    snprintf(s_ai_server_url, sizeof(s_ai_server_url), "%s", url);
    update_ota_manifest_url();
    save_ai_home_config();
    printf("ok ai_home_server=\"%s\"\n", s_ai_server_url);
}

static void ai_home_autostart_command(const char* args) {
    bool enabled = false;
    if (!parse_on_off(args, &enabled)) {
        printf("err use: ai_home_autostart on|off\n");
        return;
    }
    s_ai_home_autostart = enabled;
    save_ai_home_config();
    printf("ok ai_home_autostart=%d\n", enabled);
}

static void wake_command(const char* args) {
    char phrase[sizeof(s_last_wake_phrase)] = {};
    snprintf(phrase, sizeof(phrase), "%s", args == NULL ? "" : args);
    trim_line(phrase);
    if (phrase[0] == '\0') {
        snprintf(phrase, sizeof(phrase), "ouo");
    }

    float confidence = 0.80f;
    char* last_space = strrchr(phrase, ' ');
    if (last_space != NULL) {
        char* end = NULL;
        float parsed = strtof(last_space + 1, &end);
        if (end != last_space + 1 && *end == '\0' && parsed >= 0.0f && parsed <= 1.0f) {
            confidence = parsed;
            *last_space = '\0';
            trim_line(phrase);
        }
    }

    snprintf(s_last_wake_phrase, sizeof(s_last_wake_phrase), "%s", phrase);
    s_last_wake_confidence = (uint8_t)(confidence * 100.0f);
    apply_mood_from_ai(confidence >= 0.80f ? "blink" : "surprise");
    ai_home_post_wake_event(s_last_wake_phrase, confidence);
    printf("wake ok phrase=\"%s\" confidence=%.2f mood=%s post=%s/api/v1/wake\n",
           s_last_wake_phrase,
           confidence,
           s_state.mood,
           s_ai_server_url);
}

static void emotion_map_command(const char* args) {
    char text[160] = {};
    snprintf(text, sizeof(text), "%s", args == NULL ? "" : args);
    trim_line(text);
    if (text[0] == '\0') {
        printf("err use: emotion_map <text>\n");
        return;
    }
    lower_line(text);
    uint8_t confidence = 0;
    const char* mood = emotion_for_text(text, &confidence);
    apply_mood_from_ai(mood);
    printf("emotion_map mood=%s confidence=%u text=\"%s\"\n", mood, confidence, text);
}

static void ota_manifest_url_command(const char* args) {
    char url[sizeof(s_ota_manifest_url)] = {};
    snprintf(url, sizeof(url), "%s", args == NULL ? "" : args);
    trim_line(url);
    if (url[0] == '\0') {
        printf("err use: ota_manifest_url <url>\n");
        return;
    }
    snprintf(s_ota_manifest_url, sizeof(s_ota_manifest_url), "%s", url);
    save_ai_home_config();
    printf("ok ota_manifest_url=\"%s\"\n", s_ota_manifest_url);
}

static esp_err_t ensure_ouo_renderer_ready(void) {
    if (s_state.renderer_ready) {
        return ESP_OK;
    }

    esp_err_t err = ensure_korvo_lcd_ready();
    if (err != ESP_OK) {
        return err;
    }

    uint16_t* fb0 = NULL;
    uint16_t* fb1 = NULL;
    err = esp_lcd_rgb_panel_get_frame_buffer(s_lcd_panel, 2, (void**)&fb0, (void**)&fb1);
    if (err != ESP_OK || fb0 == NULL || fb1 == NULL) {
        return err == ESP_OK ? ESP_FAIL : err;
    }

    if (!ouo_s31_renderer_begin(s_lcd_panel, fb0, fb1, OUO_KORVO_LCD_H_RES, OUO_KORVO_LCD_V_RES)) {
        return ESP_FAIL;
    }
    s_state.renderer_ready = true;
    s_state.display_mode = "ouo_runtime_rgb";
    return ESP_OK;
}

static void renderer_test_command(void) {
    printf("renderer_test begin profile=korvo_1 renderer=ouo_runtime_rgb\n");
    esp_err_t err = ensure_ouo_renderer_ready();
    if (err != ESP_OK) {
        printf("renderer_test err=%s\n", esp_err_to_name(err));
        return;
    }
    ouo_s31_renderer_tick((uint32_t)(esp_timer_get_time() / 1000));
    printf("renderer_test ok hres=%d vres=%d frames=%" PRIu32 " mood=%s pclk=%d\n",
           OUO_KORVO_LCD_H_RES,
           OUO_KORVO_LCD_V_RES,
           ouo_s31_renderer_frame_count(),
           ouo_s31_renderer_mood_name(),
           OUO_KORVO_LCD_PCLK_HZ);
}

static void print_board_info(void) {
    printf("board_profiles=function_coreboard_1,korvo_1\n");
    printf("function_coreboard_1: usb_serial_jtag=present usb_uart=present ethernet=present audio=es8311 rgb_led_gpio=%d\n", OUO_FUNCTION_RGB_LED_GPIO);
    printf("korvo_1: usb_uart=present audio=present lcd=rgb_800x480_st7262e43 camera=present microsd=present rgb_led_gpio=%d\n", OUO_KORVO_RGB_LED_GPIO);
    printf("active_profile=korvo_1 rgb_led_test=korvo_led_test default_gpio=%d gpio_override=\"korvo_led_test <gpio>\"\n", OUO_KORVO_RGB_LED_GPIO);
}

static esp_err_t ensure_korvo_lcd_ready(void) {
    if (s_lcd_panel != NULL) {
        return ESP_OK;
    }

    esp_lcd_rgb_panel_config_t panel_config = {
        .clk_src = LCD_CLK_SRC_DEFAULT,
        .timings = {
            .pclk_hz = OUO_KORVO_LCD_PCLK_HZ,
            .h_res = OUO_KORVO_LCD_H_RES,
            .v_res = OUO_KORVO_LCD_V_RES,
            .hsync_pulse_width = 1,
            .hsync_back_porch = 40,
            .hsync_front_porch = 20,
            .vsync_pulse_width = 1,
            .vsync_back_porch = 10,
            .vsync_front_porch = 5,
            .flags = {
                .pclk_active_neg = true,
            },
        },
        .data_width = 16,
        .in_color_format = LCD_COLOR_FMT_RGB565,
        .out_color_format = LCD_COLOR_FMT_RGB565,
        .num_fbs = 2,
        .bounce_buffer_size_px = 0,
        .dma_burst_size = 64,
        .hsync_gpio_num = 44,
        .vsync_gpio_num = 45,
        .de_gpio_num = 43,
        .pclk_gpio_num = 40,
        .disp_gpio_num = -1,
        .data_gpio_nums = {
            8, 9, 10, 11, 12, 13, 14, 15,
            16, 17, 18, 19, 33, 34, 35, 36,
        },
        .flags = {
            .fb_in_psram = true,
        },
    };

    esp_err_t err = esp_lcd_new_rgb_panel(&panel_config, &s_lcd_panel);
    if (err != ESP_OK) {
        s_lcd_panel = NULL;
        return err;
    }

    err = esp_lcd_panel_reset(s_lcd_panel);
    if (err != ESP_OK) {
        return err;
    }
    err = esp_lcd_panel_init(s_lcd_panel);
    if (err == ESP_OK) {
        esp_err_t disp_err = esp_lcd_panel_disp_on_off(s_lcd_panel, true);
        if (disp_err != ESP_OK && disp_err != ESP_ERR_NOT_SUPPORTED) {
            err = disp_err;
        }
    }
    if (err == ESP_OK) {
        s_state.lcd_ready = true;
    }
    return err;
}

static uint16_t lcd_test_pixel(int x, int y) {
    static const uint16_t bars[] = {
        0xf800, 0x07e0, 0x001f, 0xffe0, 0xf81f, 0x07ff, 0xffff, 0x0000,
    };
    if (y >= OUO_KORVO_LCD_V_RES - 40) {
        return ((x / 16) % 2) ? 0xffff : 0x0000;
    }
    return bars[(x * (int)(sizeof(bars) / sizeof(bars[0]))) / OUO_KORVO_LCD_H_RES];
}

static void lcd_test_command(void) {
    printf("lcd_test begin profile=korvo_1 panel=st7262e43_rgb_800x480\n");
    esp_err_t err = ensure_korvo_lcd_ready();
    if (err != ESP_OK) {
        printf("lcd_test err=%s step=init\n", esp_err_to_name(err));
        return;
    }

    uint16_t* fb = NULL;
    err = esp_lcd_rgb_panel_get_frame_buffer(s_lcd_panel, 1, (void**)&fb);
    if (err != ESP_OK || fb == NULL) {
        printf("lcd_test err=%s step=get_frame_buffer\n", esp_err_to_name(err));
        return;
    }

    for (int y = 0; y < OUO_KORVO_LCD_V_RES; ++y) {
        for (int x = 0; x < OUO_KORVO_LCD_H_RES; ++x) {
            fb[y * OUO_KORVO_LCD_H_RES + x] = lcd_test_pixel(x, y);
        }
    }
    esp_cache_msync(fb, OUO_KORVO_LCD_H_RES * OUO_KORVO_LCD_V_RES * sizeof(uint16_t), ESP_CACHE_MSYNC_FLAG_DIR_C2M);
    esp_lcd_panel_draw_bitmap(s_lcd_panel, 0, 0, OUO_KORVO_LCD_H_RES, OUO_KORVO_LCD_V_RES, fb);
    s_state.display_mode = "lcd_color_bars";
    printf("lcd_test ok hres=%d vres=%d pclk=%d psram=%" PRIu32 "\n",
           OUO_KORVO_LCD_H_RES,
           OUO_KORVO_LCD_V_RES,
           OUO_KORVO_LCD_PCLK_HZ,
           (uint32_t)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
}

static void lcd_probe_command(void) {
    printf("lcd_probe begin profile=korvo_1 pattern=black_white_frame\n");
    esp_err_t err = ensure_korvo_lcd_ready();
    if (err != ESP_OK) {
        printf("lcd_probe err=%s step=init\n", esp_err_to_name(err));
        return;
    }

    uint16_t* fb = NULL;
    err = esp_lcd_rgb_panel_get_frame_buffer(s_lcd_panel, 1, (void**)&fb);
    if (err != ESP_OK || fb == NULL) {
        printf("lcd_probe err=%s step=get_frame_buffer\n", esp_err_to_name(err));
        return;
    }

    for (int y = 0; y < OUO_KORVO_LCD_V_RES; ++y) {
        for (int x = 0; x < OUO_KORVO_LCD_H_RES; ++x) {
            uint16_t color = 0x0000;
            if (x < 12 || x >= OUO_KORVO_LCD_H_RES - 12 || y < 12 || y >= OUO_KORVO_LCD_V_RES - 12) {
                color = 0xffff;
            } else if (x >= 360 && x < 440 && y >= 200 && y < 280) {
                color = 0xffff;
            } else if (x >= 32 && x < 112 && y >= 32 && y < 112) {
                color = 0xf800;
            } else if (x >= 128 && x < 208 && y >= 32 && y < 112) {
                color = 0x07e0;
            } else if (x >= 224 && x < 304 && y >= 32 && y < 112) {
                color = 0x001f;
            }
            fb[y * OUO_KORVO_LCD_H_RES + x] = color;
        }
    }
    esp_cache_msync(fb, OUO_KORVO_LCD_H_RES * OUO_KORVO_LCD_V_RES * sizeof(uint16_t), ESP_CACHE_MSYNC_FLAG_DIR_C2M);
    esp_lcd_panel_draw_bitmap(s_lcd_panel, 0, 0, OUO_KORVO_LCD_H_RES, OUO_KORVO_LCD_V_RES, fb);
    s_state.display_mode = "lcd_probe_black_white_frame";
    printf("lcd_probe ok hres=%d vres=%d pclk=%d\n",
           OUO_KORVO_LCD_H_RES,
           OUO_KORVO_LCD_V_RES,
           OUO_KORVO_LCD_PCLK_HZ);
}

static esp_err_t gt1151_read_reg(uint16_t reg, uint8_t* data, size_t len) {
    uint8_t addr[2] = {(uint8_t)(reg >> 8), (uint8_t)(reg & 0xff)};
    return i2c_master_transmit_receive(s_touch_dev, addr, sizeof(addr), data, len, 50);
}

static esp_err_t gt1151_write_u8(uint16_t reg, uint8_t value) {
    uint8_t data[3] = {(uint8_t)(reg >> 8), (uint8_t)(reg & 0xff), value};
    return i2c_master_transmit(s_touch_dev, data, sizeof(data), 50);
}

static esp_err_t ensure_shared_i2c_bus_ready(void) {
    if (s_i2c_bus != NULL) {
        return ESP_OK;
    }
    i2c_master_bus_config_t bus_config = {
        .i2c_port = I2C_NUM_0,
        .sda_io_num = OUO_KORVO_TOUCH_SDA_GPIO,
        .scl_io_num = OUO_KORVO_TOUCH_SCL_GPIO,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags = {
            .enable_internal_pullup = true,
        },
    };
    return i2c_new_master_bus(&bus_config, &s_i2c_bus);
}

static esp_err_t ensure_touch_ready(void) {
    if (s_state.touch_ready) {
        return ESP_OK;
    }

    esp_err_t err = ensure_shared_i2c_bus_ready();
    if (err != ESP_OK) {
        return err;
    }

    err = i2c_master_probe(s_i2c_bus, OUO_GT1151_I2C_ADDR, 100);
    if (err != ESP_OK) {
        return err;
    }

    if (s_touch_dev == NULL) {
        i2c_device_config_t dev_config = {
            .dev_addr_length = I2C_ADDR_BIT_LEN_7,
            .device_address = OUO_GT1151_I2C_ADDR,
            .scl_speed_hz = 400000,
        };
        err = i2c_master_bus_add_device(s_i2c_bus, &dev_config, &s_touch_dev);
        if (err != ESP_OK) {
            return err;
        }
    }

    gt1151_write_u8(OUO_GT1151_STATUS_REG, 0);
    s_state.touch_ready = true;
    return ESP_OK;
}

static void print_touch_status(void) {
    printf("touch=%s active=%d x=%u y=%u events=%" PRIu32 " errors=%" PRIu32 " renderer_touch=%d\n",
           s_state.touch_ready ? "gt1151_i2c" : "disabled",
           s_state.touch_active,
           (unsigned)s_state.touch_x,
           (unsigned)s_state.touch_y,
           s_state.touch_events,
           s_state.touch_errors,
           s_state.renderer_ready ? ouo_s31_renderer_is_touching() : 0);
}

static void close_touch_menu(void) {
    s_menu_open = false;
    s_selected_menu_index = -1;
    if (s_state.renderer_ready) {
        ouo_s31_renderer_set_menu(false, -1);
    }
}

static void process_touch_sample(bool pressed, uint16_t x, uint16_t y, uint32_t now_ms) {
    if (pressed || s_touch_last) {
        s_state.touch_x = x;
        s_state.touch_y = y;
    }

    if (pressed && !s_touch_last) {
        s_touch_down_ms = now_ms;
        s_selected_menu_index = -1;
        if (s_menu_open) {
            s_selected_menu_index = ouo_s31_renderer_menu_index_from_panel((float)x, (float)y);
            ouo_s31_renderer_set_menu(true, s_selected_menu_index);
            s_touch_forwarded = false;
            s_menu_candidate = false;
        } else if (ouo_s31_renderer_in_menu_trigger_zone((float)x, (float)y)) {
            s_menu_candidate = true;
            s_touch_forwarded = false;
        } else {
            s_menu_candidate = false;
            s_touch_forwarded = true;
            ouo_s31_renderer_touch_down((float)x, (float)y);
        }
    } else if (pressed && s_touch_last) {
        if (s_menu_open) {
            s_selected_menu_index = ouo_s31_renderer_menu_index_from_panel((float)x, (float)y);
            ouo_s31_renderer_set_menu(true, s_selected_menu_index);
        } else if (s_menu_candidate && now_ms - s_touch_down_ms >= OUO_MENU_LONG_PRESS_MS) {
            s_menu_open = true;
            s_selected_menu_index = ouo_s31_renderer_menu_index_from_panel((float)x, (float)y);
            ouo_s31_renderer_touch_cancel();
            ouo_s31_renderer_set_menu(true, s_selected_menu_index);
            printf("menu=open\n");
        } else if (s_touch_forwarded) {
            ouo_s31_renderer_touch_move((float)x, (float)y);
        }
    } else if (!pressed && s_touch_last) {
        if (s_menu_open) {
            if (s_selected_menu_index >= 0) {
                const char* mood = menu_mood_name(s_selected_menu_index);
                if (set_current_mood(mood)) {
                    printf("menu mood=%s\n", mood);
                }
            }
            close_touch_menu();
        } else if (s_touch_forwarded) {
            ouo_s31_renderer_touch_up((float)x, (float)y);
        } else {
            ouo_s31_renderer_touch_cancel();
        }
        s_touch_forwarded = false;
        s_menu_candidate = false;
    }

    s_touch_last = pressed;
    s_state.touch_active = pressed;
}

static void cancel_touch_state(void) {
    if (s_menu_open) {
        close_touch_menu();
    }
    if (s_touch_forwarded || s_menu_candidate || s_touch_last) {
        ouo_s31_renderer_touch_cancel();
    }
    s_touch_last = false;
    s_touch_forwarded = false;
    s_menu_candidate = false;
    s_state.touch_active = false;
}

static esp_err_t ensure_rgb_led_ready(int gpio_num) {
    if (s_led_channel != NULL && s_led_encoder != NULL) {
        if (s_led_gpio != gpio_num) {
            return ESP_ERR_INVALID_STATE;
        }
        return ESP_OK;
    }

    rmt_tx_channel_config_t tx_chan_config = {
        .clk_src = RMT_CLK_SRC_DEFAULT,
        .gpio_num = gpio_num,
        .mem_block_symbols = 64,
        .resolution_hz = OUO_RMT_LED_STRIP_RESOLUTION_HZ,
        .trans_queue_depth = 4,
    };
    esp_err_t err = rmt_new_tx_channel(&tx_chan_config, &s_led_channel);
    if (err != ESP_OK) {
        return err;
    }

    led_strip_encoder_config_t encoder_config = {
        .resolution = OUO_RMT_LED_STRIP_RESOLUTION_HZ,
    };
    err = rmt_new_led_strip_encoder(&encoder_config, &s_led_encoder);
    if (err != ESP_OK) {
        return err;
    }

    err = rmt_enable(s_led_channel);
    if (err == ESP_OK) {
        s_led_gpio = gpio_num;
    }
    return err;
}

static esp_err_t rgb_led_set(int gpio_num, uint8_t red, uint8_t green, uint8_t blue) {
    esp_err_t err = ensure_rgb_led_ready(gpio_num);
    if (err != ESP_OK) {
        return err;
    }

    uint8_t pixel[3] = { green, red, blue };
    rmt_transmit_config_t tx_config = {
        .loop_count = 0,
    };
    err = rmt_transmit(s_led_channel, s_led_encoder, pixel, sizeof(pixel), &tx_config);
    if (err != ESP_OK) {
        return err;
    }
    return rmt_tx_wait_all_done(s_led_channel, pdMS_TO_TICKS(1000));
}

static void run_rgb_led_test(const char* name, int gpio_num, const char* profile) {
    printf("%s begin gpio=%d profile=%s\n", name, gpio_num, profile);
    const uint8_t colors[][3] = {
        { 24, 0, 0 },
        { 0, 24, 0 },
        { 0, 0, 24 },
        { 0, 0, 0 },
    };
    for (size_t i = 0; i < sizeof(colors) / sizeof(colors[0]); ++i) {
        esp_err_t err = rgb_led_set(gpio_num, colors[i][0], colors[i][1], colors[i][2]);
        if (err != ESP_OK) {
            printf("%s err=%s step=%u\n", name, esp_err_to_name(err), (unsigned)i);
            return;
        }
        vTaskDelay(pdMS_TO_TICKS(250));
    }
    printf("%s ok\n", name);
}

static void korvo_led_test_command(int gpio_num) {
    run_rgb_led_test("korvo_led_test", gpio_num, "korvo_1");
}

static void function_led_test_command(void) {
    run_rgb_led_test("function_led_test", OUO_FUNCTION_RGB_LED_GPIO, "function_coreboard_1_only");
}

static void print_mac_line(const char* name, esp_mac_type_t type) {
    uint8_t mac[8] = {};
    esp_err_t err = esp_read_mac(mac, type);
    if (err == ESP_OK) {
        size_t len = esp_mac_addr_len_get(type);
        printf("%s=", name);
        for (size_t i = 0; i < len; ++i) {
            printf("%02x%s", mac[i], i + 1 == len ? "" : ":");
        }
        printf("\n");
    } else {
        printf("%s=err:%s\n", name, esp_err_to_name(err));
    }
}

static void print_mac_info(void) {
    print_mac_line("mac_base", ESP_MAC_BASE);
    print_mac_line("mac_wifi_sta", ESP_MAC_WIFI_STA);
    print_mac_line("mac_wifi_ap", ESP_MAC_WIFI_SOFTAP);
    print_mac_line("mac_bt", ESP_MAC_BT);
    print_mac_line("mac_eth", ESP_MAC_ETH);
    print_mac_line("mac_ieee802154", ESP_MAC_IEEE802154);
}

static void print_partition_info(void) {
    const esp_partition_t* running = esp_ota_get_running_partition();
    if (running != NULL) {
        printf("running_partition label=%s type=%u subtype=%u address=0x%08" PRIx32 " size=%" PRIu32 "\n",
               running->label,
               running->type,
               running->subtype,
               running->address,
               running->size);
    } else {
        printf("running_partition=unknown\n");
    }

    esp_partition_iterator_t it = esp_partition_find(ESP_PARTITION_TYPE_ANY, ESP_PARTITION_SUBTYPE_ANY, NULL);
    while (it != NULL) {
        const esp_partition_t* part = esp_partition_get(it);
        printf("partition label=%s type=%u subtype=%u address=0x%08" PRIx32 " size=%" PRIu32 "\n",
               part->label,
               part->type,
               part->subtype,
               part->address,
               part->size);
        it = esp_partition_next(it);
    }
    esp_partition_iterator_release(it);
}

static void ota_status_command(void) {
    const esp_partition_t* running = esp_ota_get_running_partition();
    const esp_partition_t* boot = esp_ota_get_boot_partition();
    const esp_partition_t* next = esp_ota_get_next_update_partition(NULL);
    printf("ota firmware=%s manifest_url=\"%s\"\n", OUO_FIRMWARE_VERSION, s_ota_manifest_url);
    printf("ota last_status=%s last_target_version=%s\n",
           s_last_ota_status,
           s_last_ota_version[0] != '\0' ? s_last_ota_version : "none");
    printf("ota running=%s boot=%s next_update=%s\n",
           running != NULL ? running->label : "unknown",
           boot != NULL ? boot->label : "unknown",
           next != NULL ? next->label : "none");
    if (running == NULL || next == NULL) {
        printf("ota ready=0 reason=partition_layout_missing\n");
    } else {
        printf("ota ready=1 transport=server_manifest_then_https_ota\n");
    }
}

static esp_err_t ensure_storage_ready(void) {
    if (s_storage_ready) {
        return ESP_OK;
    }

    const esp_vfs_spiffs_conf_t conf = {
        .base_path = "/storage",
        .partition_label = "storage",
        .max_files = 4,
        .format_if_mount_failed = true,
    };
    esp_err_t err = esp_vfs_spiffs_register(&conf);
    if (err == ESP_ERR_INVALID_STATE) {
        s_storage_ready = true;
        return ESP_OK;
    }
    if (err != ESP_OK) {
        return err;
    }

    s_storage_ready = true;
    return ESP_OK;
}

static void storage_test_command(void) {
    printf("storage_test begin\n");

    esp_err_t err = ensure_storage_ready();
    if (err != ESP_OK) {
        printf("storage_test mount_err=%s\n", esp_err_to_name(err));
        return;
    }

    size_t total = 0;
    size_t used = 0;
    err = esp_spiffs_info("storage", &total, &used);
    if (err != ESP_OK) {
        printf("storage_test info_err=%s\n", esp_err_to_name(err));
        return;
    }

    const char* path = "/storage/ouo_s31_selftest.txt";
    char expected[96] = {};
    snprintf(expected, sizeof(expected), "boots=%" PRIu32 " commands=%" PRIu32 " mood=%s\n",
             s_state.boot_count,
             s_state.commands,
             s_state.mood);

    FILE* file = fopen(path, "w");
    if (file == NULL) {
        printf("storage_test write_open_err\n");
        return;
    }
    fputs(expected, file);
    fclose(file);

    char actual[96] = {};
    file = fopen(path, "r");
    if (file == NULL) {
        printf("storage_test read_open_err\n");
        return;
    }
    char* read = fgets(actual, sizeof(actual), file);
    fclose(file);
    if (read == NULL) {
        printf("storage_test read_err\n");
        return;
    }

    bool matched = strcmp(expected, actual) == 0;
    size_t total_after = 0;
    size_t used_after = 0;
    esp_spiffs_info("storage", &total_after, &used_after);

    printf("storage_test total=%u used_before=%u used_after=%u match=%d\n",
           (unsigned)total,
           (unsigned)used,
           (unsigned)used_after,
           matched);
    printf("storage_test data=\"%s\"\n", actual);
}

static void print_status(void) {
    esp_chip_info_t chip;
    esp_chip_info(&chip);

    uint32_t flash_size = 0;
    esp_flash_get_size(NULL, &flash_size);

    printf("board=%s chip_model=%d cores=%d revision=%d\n",
           s_state.board,
           chip.model,
           chip.cores,
           chip.revision);
    printf("flash=%" PRIu32 " heap=%" PRIu32 " min_heap=%" PRIu32 " psram=%" PRIu32 " uptime_ms=%" PRIu32 " boot_ms=%" PRIu32 " boots=%" PRIu32 "\n",
           flash_size,
           esp_get_free_heap_size(),
           esp_get_minimum_free_heap_size(),
           (uint32_t)heap_caps_get_free_size(MALLOC_CAP_SPIRAM),
           (uint32_t)(esp_timer_get_time() / 1000),
           s_state.boot_ms,
           s_state.boot_count);
    printf("mood=%s blush=%d lock=%d static=%d stretch=%d tilt=%d commands=%" PRIu32 "\n",
           s_state.mood,
           s_state.blush,
           s_state.lock,
           s_state.static_mood,
           s_state.stretch,
           s_state.tilt,
           s_state.commands);
    printf("lcd=%s touch=%s imu=disabled renderer=%s display_mode=%s\n",
           s_state.lcd_ready ? "korvo_rgb_800x480" : "not_initialized",
           s_state.touch_ready ? "gt1151_i2c" : "disabled",
           s_state.renderer_ready ? "ouo_runtime_rgb" : "skipped",
           s_state.display_mode);
    printf("camera_preview=%d camera_new_trans=%" PRIu32 " camera_finished=%" PRIu32 "\n",
           s_camera_preview_active,
           (uint32_t)s_camera_preview_new_trans,
           (uint32_t)s_camera_preview_finished_trans);
    printf("ai_home firmware=%s server=\"%s\" ota_manifest=\"%s\" wake=\"%s\" wake_confidence=%u server_audio=required\n",
           OUO_FIRMWARE_VERSION,
           s_ai_server_url,
           s_ota_manifest_url,
           s_last_wake_phrase,
           s_last_wake_confidence);
    printf("ai_home autostart=%d heartbeat_interval_ms=%d\n",
           s_ai_home_autostart,
           OUO_AI_HEARTBEAT_INTERVAL_MS);
    printf("wifi saved_ssid=\"%s\" autoconnect=%d\n",
           s_wifi_ssid[0] != '\0' ? s_wifi_ssid : "",
           s_wifi_autoconnect);
    print_touch_status();
    if (s_state.renderer_ready) {
        printf("renderer_frames=%" PRIu32 " renderer_last_ms=%" PRIu32 " renderer_mood=%s idle=%.1f pressure=%.1f mouth=%d\n",
               ouo_s31_renderer_frame_count(),
               ouo_s31_renderer_last_frame_ms(),
               ouo_s31_renderer_mood_name(),
               ouo_s31_renderer_idle_time(),
               ouo_s31_renderer_touch_pressure(),
               ouo_s31_renderer_mouth_direction());
    }
}

static esp_err_t ensure_wifi_ready(void) {
    if (s_wifi_ready) {
        return ESP_OK;
    }

    esp_err_t err = esp_netif_init();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        return err;
    }

    err = esp_event_loop_create_default();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        return err;
    }

    if (s_sta_netif == NULL) {
        s_sta_netif = esp_netif_create_default_wifi_sta();
        if (s_sta_netif == NULL) {
            return ESP_FAIL;
        }
    }

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    err = esp_wifi_init(&cfg);
    if (err != ESP_OK) {
        return err;
    }

    err = esp_wifi_set_mode(WIFI_MODE_STA);
    if (err != ESP_OK) {
        return err;
    }

    err = esp_wifi_start();
    if (err != ESP_OK) {
        return err;
    }

    s_wifi_ready = true;
    return ESP_OK;
}

static void wifi_scan_command(void) {
    printf("wifi_scan begin\n");

    esp_err_t err = ensure_wifi_ready();
    if (err != ESP_OK) {
        printf("wifi_scan init_err=%s\n", esp_err_to_name(err));
        return;
    }

    wifi_ap_record_t* ap_info = calloc(OUO_SCAN_MAX_APS, sizeof(wifi_ap_record_t));
    if (ap_info == NULL) {
        printf("wifi_scan err=no_memory\n");
        return;
    }

    uint16_t ap_count = 0;
    uint16_t number = OUO_SCAN_MAX_APS;
    err = esp_wifi_scan_start(NULL, true);
    if (err == ESP_OK) {
        err = esp_wifi_scan_get_ap_num(&ap_count);
    }
    if (err == ESP_OK) {
        err = esp_wifi_scan_get_ap_records(&number, ap_info);
    }

    if (err != ESP_OK) {
        printf("wifi_scan err=%s\n", esp_err_to_name(err));
        free(ap_info);
        return;
    }

    printf("wifi_scan total=%u listed=%u\n", ap_count, number);
    for (uint16_t i = 0; i < number; ++i) {
        printf("ap[%u] ssid=\"%s\" rssi=%d channel=%u auth=%d\n",
               i,
               (const char*)ap_info[i].ssid,
               ap_info[i].rssi,
               ap_info[i].primary,
               ap_info[i].authmode);
    }

    free(ap_info);
}

static void wifi_status_command(void) {
    esp_err_t init_err = ensure_wifi_ready();
    if (init_err != ESP_OK) {
        printf("wifi=init_err status=%s\n", esp_err_to_name(init_err));
        return;
    }
    wifi_ap_record_t ap = {};
    esp_err_t ap_err = esp_wifi_sta_get_ap_info(&ap);
    if (ap_err == ESP_OK) {
        esp_netif_ip_info_t ip = {};
        if (s_sta_netif != NULL && esp_netif_get_ip_info(s_sta_netif, &ip) == ESP_OK) {
            printf("wifi=connected ssid=\"%s\" rssi=%d channel=%u ip=" IPSTR " gw=" IPSTR " autoconnect=%d\n",
                   (const char*)ap.ssid,
                   ap.rssi,
                   ap.primary,
                   IP2STR(&ip.ip),
                   IP2STR(&ip.gw),
                   s_wifi_autoconnect);
        } else {
            printf("wifi=connected ssid=\"%s\" rssi=%d channel=%u ip=unknown autoconnect=%d\n",
                   (const char*)ap.ssid,
                   ap.rssi,
                   ap.primary,
                   s_wifi_autoconnect);
        }
        return;
    }
    printf("wifi=disconnected status=%s saved_ssid=\"%s\" autoconnect=%d\n",
           esp_err_to_name(ap_err),
           s_wifi_ssid[0] != '\0' ? s_wifi_ssid : "",
           s_wifi_autoconnect);
}

static esp_err_t wifi_start_sta_connect(const char* ssid, const char* pass) {
    esp_err_t err = ensure_wifi_ready();
    if (err != ESP_OK) {
        return err;
    }

    wifi_config_t config = {};
    strcpy((char*)config.sta.ssid, ssid);
    strcpy((char*)config.sta.password, pass);
    config.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;
    config.sta.sae_pwe_h2e = WPA3_SAE_PWE_BOTH;

    esp_wifi_disconnect();
    err = esp_wifi_set_config(WIFI_IF_STA, &config);
    if (err == ESP_OK) {
        err = esp_wifi_connect();
    }
    return err;
}

static void wifi_connect_command(const char* args) {
    char work[128] = {};
    snprintf(work, sizeof(work), "%s", args == NULL ? "" : args);
    trim_line(work);
    char* pass = strchr(work, ' ');
    if (work[0] == '\0' || pass == NULL) {
        printf("err use: wifi_connect <ssid> <password>\n");
        return;
    }
    *pass++ = '\0';
    trim_line(pass);
    if (pass[0] == '\0') {
        printf("err use: wifi_connect <ssid> <password>\n");
        return;
    }
    if (strlen(work) >= sizeof(((wifi_config_t*)0)->sta.ssid) || strlen(pass) >= sizeof(((wifi_config_t*)0)->sta.password)) {
        printf("wifi_connect err=ssid_or_password_too_long\n");
        return;
    }

    esp_err_t err = wifi_start_sta_connect(work, pass);
    if (err != ESP_OK) {
        printf("wifi_connect err=%s\n", esp_err_to_name(err));
        return;
    }

    printf("wifi_connect connecting ssid=\"%s\"\n", work);
    for (int i = 0; i < 50; ++i) {
        vTaskDelay(pdMS_TO_TICKS(200));
        wifi_ap_record_t ap = {};
        if (esp_wifi_sta_get_ap_info(&ap) == ESP_OK) {
            memcpy(s_wifi_ssid, work, strlen(work) + 1);
            memcpy(s_wifi_password, pass, strlen(pass) + 1);
            s_wifi_autoconnect = true;
            save_wifi_config();
            wifi_status_command();
            return;
        }
    }
    printf("wifi_connect timeout\n");
}

static void wifi_autoconnect_command(const char* args) {
    bool enabled = false;
    if (!parse_on_off(args, &enabled)) {
        printf("err use: wifi_autoconnect on|off\n");
        return;
    }
    s_wifi_autoconnect = enabled;
    save_wifi_config();
    printf("ok wifi_autoconnect=%d saved_ssid=\"%s\"\n",
           enabled,
           s_wifi_ssid[0] != '\0' ? s_wifi_ssid : "");
}

static void wifi_forget_command(void) {
    esp_wifi_disconnect();
    clear_wifi_config();
    printf("ok wifi_forget\n");
}

static void wifi_autoconnect_start(void) {
    if (!s_wifi_autoconnect || s_wifi_ssid[0] == '\0') {
        ESP_LOGI(TAG, "wifi autoconnect skipped enabled=%d saved=%d", s_wifi_autoconnect, s_wifi_ssid[0] != '\0');
        return;
    }
    esp_err_t err = wifi_start_sta_connect(s_wifi_ssid, s_wifi_password);
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "wifi autoconnect started ssid=\"%s\"", s_wifi_ssid);
    } else {
        ESP_LOGW(TAG, "wifi autoconnect failed err=%s ssid=\"%s\"", esp_err_to_name(err), s_wifi_ssid);
    }
}

static bool wifi_is_connected(void) {
    wifi_ap_record_t ap = {};
    return esp_wifi_sta_get_ap_info(&ap) == ESP_OK;
}

static esp_err_t http_event_collect_response(esp_http_client_event_t* evt) {
    if (evt->event_id != HTTP_EVENT_ON_DATA || evt->data == NULL || evt->data_len <= 0) {
        return ESP_OK;
    }
    ouo_http_response_t* response = (ouo_http_response_t*)evt->user_data;
    if (response == NULL || response->data == NULL || response->cap <= 0) {
        return ESP_OK;
    }
    int copy = evt->data_len;
    if (response->len + copy >= response->cap) {
        copy = response->cap - response->len - 1;
    }
    if (copy > 0) {
        memcpy(response->data + response->len, evt->data, copy);
        response->len += copy;
        response->data[response->len] = '\0';
    }
    return ESP_OK;
}

static esp_err_t http_event_sha256_download(esp_http_client_event_t* evt) {
    if (evt->event_id != HTTP_EVENT_ON_DATA || evt->data == NULL || evt->data_len <= 0) {
        return ESP_OK;
    }
    ouo_sha256_download_t* download = (ouo_sha256_download_t*)evt->user_data;
    if (download == NULL || download->op == NULL) {
        return ESP_OK;
    }
    psa_status_t status = psa_hash_update(download->op, (const uint8_t*)evt->data, (size_t)evt->data_len);
    if (status != PSA_SUCCESS) {
        download->err = ESP_FAIL;
        return ESP_FAIL;
    }
    download->bytes += (uint32_t)evt->data_len;
    return ESP_OK;
}

static void build_ai_url(char* out, size_t out_size, const char* path) {
    const char* slash = s_ai_server_url[strlen(s_ai_server_url) - 1] == '/' ? "" : "/";
    while (path[0] == '/') {
        path++;
    }
    snprintf(out, out_size, "%s%s%s", s_ai_server_url, slash, path);
}

static void json_escape(char* out, size_t out_size, const char* input) {
    size_t used = 0;
    if (out_size == 0) {
        return;
    }
    for (const unsigned char* p = (const unsigned char*)input; *p != '\0' && used + 1 < out_size; ++p) {
        if (*p == '"' || *p == '\\') {
            if (used + 2 >= out_size) {
                break;
            }
            out[used++] = '\\';
            out[used++] = (char)*p;
        } else if (*p == '\n' || *p == '\r' || *p == '\t') {
            if (used + 2 >= out_size) {
                break;
            }
            out[used++] = '\\';
            out[used++] = *p == '\n' ? 'n' : (*p == '\r' ? 'r' : 't');
        } else if (*p >= 0x20) {
            out[used++] = (char)*p;
        }
    }
    out[used] = '\0';
}

static bool is_hex_sha256(const char* value) {
    if (value == NULL || strlen(value) != 64) {
        return false;
    }
    for (const char* p = value; *p != '\0'; ++p) {
        if (!isxdigit((unsigned char)*p)) {
            return false;
        }
    }
    return true;
}

static void sha256_to_hex(const unsigned char digest[32], char* out, size_t out_size) {
    static const char hex[] = "0123456789abcdef";
    if (out == NULL || out_size < 65) {
        return;
    }
    for (int i = 0; i < 32; ++i) {
        out[i * 2] = hex[(digest[i] >> 4) & 0x0f];
        out[i * 2 + 1] = hex[digest[i] & 0x0f];
    }
    out[64] = '\0';
}

static bool sha256_hex_equal(const char* expected, const char* actual) {
    if (!is_hex_sha256(expected) || !is_hex_sha256(actual)) {
        return false;
    }
    for (int i = 0; i < 64; ++i) {
        if (tolower((unsigned char)expected[i]) != tolower((unsigned char)actual[i])) {
            return false;
        }
    }
    return true;
}

static bool json_extract_string(const char* json, const char* key, char* out, size_t out_size) {
    if (json == NULL || key == NULL || out == NULL || out_size == 0) {
        return false;
    }
    char pattern[48] = {};
    snprintf(pattern, sizeof(pattern), "\"%s\"", key);
    const char* cursor = strstr(json, pattern);
    if (cursor == NULL) {
        return false;
    }
    cursor = strchr(cursor + strlen(pattern), ':');
    if (cursor == NULL) {
        return false;
    }
    cursor++;
    while (*cursor != '\0' && isspace((unsigned char)*cursor)) {
        cursor++;
    }
    if (*cursor != '"') {
        return false;
    }
    cursor++;

    size_t used = 0;
    while (*cursor != '\0' && *cursor != '"' && used + 1 < out_size) {
        if (*cursor == '\\' && cursor[1] != '\0') {
            cursor++;
            if (*cursor == 'n') {
                out[used++] = '\n';
            } else if (*cursor == 'r') {
                out[used++] = '\r';
            } else if (*cursor == 't') {
                out[used++] = '\t';
            } else {
                out[used++] = *cursor;
            }
        } else {
            out[used++] = *cursor;
        }
        cursor++;
    }
    out[used] = '\0';
    return used > 0 || *cursor == '"';
}

static bool json_extract_u32(const char* json, const char* key, uint32_t* out) {
    if (json == NULL || key == NULL || out == NULL) {
        return false;
    }
    char pattern[48] = {};
    snprintf(pattern, sizeof(pattern), "\"%s\"", key);
    const char* cursor = strstr(json, pattern);
    if (cursor == NULL) {
        return false;
    }
    cursor = strchr(cursor + strlen(pattern), ':');
    if (cursor == NULL) {
        return false;
    }
    cursor++;
    while (*cursor != '\0' && isspace((unsigned char)*cursor)) {
        cursor++;
    }
    char* end = NULL;
    unsigned long value = strtoul(cursor, &end, 10);
    if (end == cursor) {
        return false;
    }
    *out = (uint32_t)value;
    return true;
}

static esp_err_t ai_home_get_url(const char* url, char* response, size_t response_size, int* status_code) {
    if (status_code != NULL) {
        *status_code = 0;
    }
    if (response != NULL && response_size > 0) {
        response[0] = '\0';
    }

    esp_err_t err = ensure_wifi_ready();
    if (err != ESP_OK) {
        return err;
    }
    if (!wifi_is_connected()) {
        return ESP_ERR_WIFI_NOT_CONNECT;
    }

    ouo_http_response_t collector = {
        .data = response,
        .len = 0,
        .cap = (int)response_size,
    };
    esp_http_client_config_t config = {
        .url = url,
        .method = HTTP_METHOD_GET,
        .timeout_ms = OUO_AI_HTTP_TIMEOUT_MS,
        .event_handler = http_event_collect_response,
        .user_data = &collector,
    };
    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (client == NULL) {
        return ESP_ERR_NO_MEM;
    }
    err = esp_http_client_perform(client);
    if (status_code != NULL) {
        *status_code = esp_http_client_get_status_code(client);
    }
    esp_http_client_cleanup(client);
    return err;
}

static esp_err_t ai_home_post_json_timeout(const char* path,
                                           const char* body,
                                           char* response,
                                           size_t response_size,
                                           int* status_code,
                                           int timeout_ms) {
    if (status_code != NULL) {
        *status_code = 0;
    }
    if (response != NULL && response_size > 0) {
        response[0] = '\0';
    }

    esp_err_t err = ensure_wifi_ready();
    if (err != ESP_OK) {
        return err;
    }
    if (!wifi_is_connected()) {
        return ESP_ERR_WIFI_NOT_CONNECT;
    }

    char url[192] = {};
    build_ai_url(url, sizeof(url), path);
    ouo_http_response_t collector = {
        .data = response,
        .len = 0,
        .cap = (int)response_size,
    };
    esp_http_client_config_t config = {
        .url = url,
        .method = HTTP_METHOD_POST,
        .timeout_ms = timeout_ms,
        .event_handler = http_event_collect_response,
        .user_data = &collector,
    };
    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (client == NULL) {
        return ESP_ERR_NO_MEM;
    }

    esp_http_client_set_header(client, "Content-Type", "application/json");
    esp_http_client_set_post_field(client, body, (int)strlen(body));
    err = esp_http_client_perform(client);
    if (status_code != NULL) {
        *status_code = esp_http_client_get_status_code(client);
    }
    esp_http_client_cleanup(client);
    return err;
}

static esp_err_t ai_home_post_json(const char* path, const char* body, char* response, size_t response_size, int* status_code) {
    return ai_home_post_json_timeout(path, body, response, response_size, status_code, OUO_AI_HTTP_TIMEOUT_MS);
}

static esp_err_t ai_home_fetch_ota_manifest(ouo_ota_manifest_t* manifest, char* raw, size_t raw_size, int* status_code) {
    if (manifest == NULL || raw == NULL || raw_size == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    memset(manifest, 0, sizeof(*manifest));
    esp_err_t err = ai_home_get_url(s_ota_manifest_url, raw, raw_size, status_code);
    if (err != ESP_OK) {
        return err;
    }
    if (status_code != NULL && (*status_code < 200 || *status_code >= 300)) {
        return ESP_FAIL;
    }
    bool ok = json_extract_string(raw, "version", manifest->version, sizeof(manifest->version));
    ok = json_extract_string(raw, "firmware_url", manifest->firmware_url, sizeof(manifest->firmware_url)) && ok;
    json_extract_string(raw, "channel", manifest->channel, sizeof(manifest->channel));
    json_extract_string(raw, "sha256", manifest->sha256, sizeof(manifest->sha256));
    json_extract_u32(raw, "size", &manifest->size);
    if (!ok || manifest->firmware_url[0] == '\0') {
        return ESP_ERR_INVALID_RESPONSE;
    }
    if (manifest->size == 0 || !is_hex_sha256(manifest->sha256)) {
        return ESP_ERR_INVALID_RESPONSE;
    }
    return ESP_OK;
}

static esp_err_t ota_verify_firmware_sha256(const ouo_ota_manifest_t* manifest, char* actual_sha256, size_t actual_sha256_size) {
    if (manifest == NULL || actual_sha256 == NULL || actual_sha256_size < 65) {
        return ESP_ERR_INVALID_ARG;
    }
    actual_sha256[0] = '\0';

    esp_err_t err = ensure_wifi_ready();
    if (err != ESP_OK) {
        return err;
    }
    if (!wifi_is_connected()) {
        return ESP_ERR_WIFI_NOT_CONNECT;
    }

    psa_status_t psa_status = psa_crypto_init();
    if (psa_status != PSA_SUCCESS) {
        return ESP_FAIL;
    }

    psa_hash_operation_t sha_op = PSA_HASH_OPERATION_INIT;
    psa_status = psa_hash_setup(&sha_op, PSA_ALG_SHA_256);
    if (psa_status != PSA_SUCCESS) {
        psa_hash_abort(&sha_op);
        return ESP_FAIL;
    }

    ouo_sha256_download_t download = {
        .op = &sha_op,
        .bytes = 0,
        .err = ESP_OK,
    };
    esp_http_client_config_t config = {
        .url = manifest->firmware_url,
        .method = HTTP_METHOD_GET,
        .timeout_ms = OUO_OTA_HTTP_TIMEOUT_MS,
        .event_handler = http_event_sha256_download,
        .user_data = &download,
        .keep_alive_enable = true,
    };
    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (client == NULL) {
        psa_hash_abort(&sha_op);
        return ESP_ERR_NO_MEM;
    }

    err = esp_http_client_perform(client);
    int http_status = esp_http_client_get_status_code(client);
    esp_http_client_cleanup(client);
    if (err != ESP_OK) {
        psa_hash_abort(&sha_op);
        return err;
    }
    if (download.err != ESP_OK || http_status < 200 || http_status >= 300) {
        psa_hash_abort(&sha_op);
        return ESP_FAIL;
    }
    if (manifest->size > 0 && download.bytes != manifest->size) {
        psa_hash_abort(&sha_op);
        ESP_LOGW(TAG, "ota sha256 size mismatch expected=%" PRIu32 " actual=%" PRIu32, manifest->size, download.bytes);
        return ESP_ERR_INVALID_RESPONSE;
    }

    unsigned char digest[32] = {};
    size_t digest_len = 0;
    psa_status = psa_hash_finish(&sha_op, digest, sizeof(digest), &digest_len);
    if (psa_status != PSA_SUCCESS || digest_len != sizeof(digest)) {
        psa_hash_abort(&sha_op);
        return ESP_FAIL;
    }

    sha256_to_hex(digest, actual_sha256, actual_sha256_size);
    if (!sha256_hex_equal(manifest->sha256, actual_sha256)) {
        ESP_LOGW(TAG, "ota sha256 mismatch expected=%s actual=%s", manifest->sha256, actual_sha256);
        return ESP_ERR_INVALID_RESPONSE;
    }
    return ESP_OK;
}

static void ai_home_report_ota(const char* status, const char* target_version, const char* detail) {
    char escaped_detail[160] = {};
    char escaped_target[64] = {};
    json_escape(escaped_detail, sizeof(escaped_detail), detail == NULL ? "" : detail);
    json_escape(escaped_target, sizeof(escaped_target), target_version == NULL ? "" : target_version);
    char body[384] = {};
    snprintf(body, sizeof(body),
             "{\"device_id\":\"ouo-s31-korvo-1\",\"current_version\":\"%s\",\"target_version\":\"%s\",\"status\":\"%s\",\"detail\":\"%s\"}",
             OUO_FIRMWARE_VERSION,
             escaped_target,
             status == NULL ? "unknown" : status,
             escaped_detail);

    char response[512] = {};
    int status_code = 0;
    esp_err_t err = ai_home_post_json("/api/v1/ota/report", body, response, sizeof(response), &status_code);
    if (err != ESP_OK || status_code < 200 || status_code >= 300) {
        ESP_LOGW(TAG, "ai_home ota report failed err=%s http=%d", esp_err_to_name(err), status_code);
    }
}

static void ota_check_command(void) {
    char raw[OUO_AI_HTTP_RESPONSE_MAX] = {};
    int status_code = 0;
    ouo_ota_manifest_t manifest = {};
    esp_err_t err = ai_home_fetch_ota_manifest(&manifest, raw, sizeof(raw), &status_code);
    if (err != ESP_OK) {
        snprintf(s_last_ota_status, sizeof(s_last_ota_status), "check_failed");
        printf("ota_check err=%s http=%d manifest_url=\"%s\" response=\"%s\"\n",
               esp_err_to_name(err),
               status_code,
               s_ota_manifest_url,
               raw);
        ai_home_report_ota("check_failed", NULL, esp_err_to_name(err));
        return;
    }

    snprintf(s_last_ota_status, sizeof(s_last_ota_status), "manifest_ok");
    snprintf(s_last_ota_version, sizeof(s_last_ota_version), "%s", manifest.version);
    printf("ota_check ok http=%d version=%s channel=%s size=%" PRIu32 " sha256=%s sha256_valid=1 firmware_url=\"%s\"\n",
           status_code,
           manifest.version,
           manifest.channel[0] != '\0' ? manifest.channel : "unknown",
           manifest.size,
           manifest.sha256[0] != '\0' ? manifest.sha256 : "none",
           manifest.firmware_url);
    ai_home_report_ota("manifest_ok", manifest.version, manifest.firmware_url);
}

static void ota_update_command(void) {
    char raw[OUO_AI_HTTP_RESPONSE_MAX] = {};
    int status_code = 0;
    ouo_ota_manifest_t manifest = {};
    esp_err_t err = ai_home_fetch_ota_manifest(&manifest, raw, sizeof(raw), &status_code);
    if (err != ESP_OK) {
        snprintf(s_last_ota_status, sizeof(s_last_ota_status), "manifest_failed");
        printf("ota_update manifest_err=%s http=%d response=\"%s\"\n", esp_err_to_name(err), status_code, raw);
        ai_home_report_ota("manifest_failed", NULL, esp_err_to_name(err));
        return;
    }

    snprintf(s_last_ota_status, sizeof(s_last_ota_status), "verifying");
    snprintf(s_last_ota_version, sizeof(s_last_ota_version), "%s", manifest.version);
    printf("ota_update verify version=%s size=%" PRIu32 " sha256=%s url=\"%s\"\n",
           manifest.version,
           manifest.size,
           manifest.sha256,
           manifest.firmware_url);
    ai_home_report_ota("verifying", manifest.version, manifest.firmware_url);

    char actual_sha256[65] = {};
    err = ota_verify_firmware_sha256(&manifest, actual_sha256, sizeof(actual_sha256));
    if (err != ESP_OK) {
        snprintf(s_last_ota_status, sizeof(s_last_ota_status), "verify_failed");
        printf("ota_update verify_err=%s expected_sha256=%s actual_sha256=%s\n",
               esp_err_to_name(err),
               manifest.sha256,
               actual_sha256[0] != '\0' ? actual_sha256 : "none");
        ai_home_report_ota("verify_failed", manifest.version, esp_err_to_name(err));
        return;
    }

    snprintf(s_last_ota_status, sizeof(s_last_ota_status), "downloading");
    printf("ota_update begin version=%s size=%" PRIu32 " sha256=%s url=\"%s\"\n",
           manifest.version,
           manifest.size,
           actual_sha256,
           manifest.firmware_url);
    ai_home_report_ota("downloading", manifest.version, manifest.firmware_url);

    esp_http_client_config_t http_config = {
        .url = manifest.firmware_url,
        .timeout_ms = OUO_OTA_HTTP_TIMEOUT_MS,
        .keep_alive_enable = true,
    };
    esp_https_ota_config_t ota_config = {
        .http_config = &http_config,
    };
    err = esp_https_ota(&ota_config);
    if (err != ESP_OK) {
        snprintf(s_last_ota_status, sizeof(s_last_ota_status), "failed");
        printf("ota_update err=%s\n", esp_err_to_name(err));
        ai_home_report_ota("failed", manifest.version, esp_err_to_name(err));
        return;
    }

    snprintf(s_last_ota_status, sizeof(s_last_ota_status), "rebooting");
    printf("ota_update ok version=%s rebooting\n", manifest.version);
    ai_home_report_ota("rebooting", manifest.version, "ota applied");
    fflush(stdout);
    vTaskDelay(pdMS_TO_TICKS(250));
    esp_restart();
}

static void ai_home_post_wake_event(const char* phrase, float confidence) {
    if (!s_wifi_ready || !wifi_is_connected()) {
        return;
    }

    char escaped[128] = {};
    json_escape(escaped, sizeof(escaped), phrase == NULL ? "" : phrase);
    char body[256] = {};
    snprintf(body, sizeof(body),
             "{\"device_id\":\"ouo-s31-korvo-1\",\"phrase\":\"%s\",\"confidence\":%.2f}",
             escaped,
             confidence);

    char response[512] = {};
    int status_code = 0;
    esp_err_t err = ai_home_post_json("/api/v1/wake", body, response, sizeof(response), &status_code);
    if (err != ESP_OK || status_code < 200 || status_code >= 300) {
        ESP_LOGW(TAG, "ai_home wake post failed err=%s http=%d", esp_err_to_name(err), status_code);
        return;
    }
    ESP_LOGI(TAG, "ai_home wake post ok http=%d", status_code);
}

static void ai_home_ping_command(bool verbose) {
    esp_netif_ip_info_t ip = {};
    bool has_ip = s_sta_netif != NULL && esp_netif_get_ip_info(s_sta_netif, &ip) == ESP_OK;
    char body[384] = {};
    if (has_ip) {
        snprintf(body, sizeof(body),
                 "{\"device_id\":\"ouo-s31-korvo-1\",\"firmware_version\":\"%s\",\"ip\":\"" IPSTR "\",\"battery_percent\":null,\"current_mood\":\"%s\"}",
                 OUO_FIRMWARE_VERSION,
                 IP2STR(&ip.ip),
                 s_state.mood);
    } else {
        snprintf(body, sizeof(body),
                 "{\"device_id\":\"ouo-s31-korvo-1\",\"firmware_version\":\"%s\",\"ip\":null,\"battery_percent\":null,\"current_mood\":\"%s\"}",
                 OUO_FIRMWARE_VERSION,
                 s_state.mood);
    }

    char response[OUO_AI_HTTP_RESPONSE_MAX] = {};
    int status_code = 0;
    esp_err_t err = ai_home_post_json("/api/v1/device/heartbeat", body, response, sizeof(response), &status_code);
    if (err != ESP_OK || status_code < 200 || status_code >= 300) {
        if (verbose) {
            printf("ai_home_ping err=%s http=%d response=\"%s\"\n", esp_err_to_name(err), status_code, response);
        } else {
            ESP_LOGW(TAG, "ai_home heartbeat failed err=%s http=%d", esp_err_to_name(err), status_code);
        }
        return;
    }
    if (verbose) {
        printf("ai_home_ping ok http=%d response=\"%s\"\n", status_code, response);
    } else {
        ESP_LOGI(TAG, "ai_home heartbeat ok http=%d", status_code);
    }
}

static bool ai_home_apply_action_response(const char* response) {
    char kind[32] = {};
    char value[32] = {};
    if (!json_extract_string(response, "kind", kind, sizeof(kind)) ||
        !json_extract_string(response, "value", value, sizeof(value))) {
        return false;
    }
    if (strcmp(kind, "set_mood") == 0 && valid_mood(value)) {
        apply_mood_from_ai(value);
        return true;
    }
    if (strcmp(kind, "capture_camera") == 0 && strcmp(value, "snapshot") == 0) {
        ai_home_camera_snapshot_command();
        return true;
    }
    if (strcmp(kind, "ota_check") == 0 && strcmp(value, "manifest") == 0) {
        ota_check_command();
        return true;
    }
    if (strcmp(kind, "ota_update") == 0 && strcmp(value, "apply") == 0) {
        ota_update_command();
        return true;
    }
    ESP_LOGW(TAG, "ai_home ignored action kind=%s value=%s", kind, value);
    return false;
}

static void ai_home_poll_command(bool verbose) {
    char body[96] = {};
    snprintf(body, sizeof(body), "{\"device_id\":\"ouo-s31-korvo-1\"}");

    char response[OUO_AI_HTTP_RESPONSE_MAX] = {};
    int status_code = 0;
    esp_err_t err = ai_home_post_json("/api/v1/device/command", body, response, sizeof(response), &status_code);
    if (err != ESP_OK || status_code < 200 || status_code >= 300) {
        if (verbose) {
            printf("ai_home_poll err=%s http=%d response=\"%s\"\n", esp_err_to_name(err), status_code, response);
        } else {
            ESP_LOGW(TAG, "ai_home command poll failed err=%s http=%d", esp_err_to_name(err), status_code);
        }
        return;
    }

    bool applied = ai_home_apply_action_response(response);
    if (verbose) {
        printf("ai_home_poll ok http=%d applied=%d mood=%s response=\"%s\"\n",
               status_code,
               applied,
               s_state.mood,
               response);
    } else if (applied) {
        ESP_LOGI(TAG, "ai_home command applied mood=%s", s_state.mood);
    }
}

static void ai_home_dialog_command(const char* args) {
    char text[192] = {};
    snprintf(text, sizeof(text), "%s", args == NULL ? "" : args);
    trim_line(text);
    if (text[0] == '\0') {
        printf("err use: ai_home_dialog <text>\n");
        return;
    }

    char escaped[384] = {};
    json_escape(escaped, sizeof(escaped), text);
    char body[768] = {};
    snprintf(body, sizeof(body),
             "{\"device_id\":\"ouo-s31-korvo-1\",\"text\":\"%s\",\"locale\":\"zh-CN\",\"context\":\"mood=%s wake=%s\"}",
             escaped,
             s_state.mood,
             s_last_wake_phrase);

    char response[OUO_AI_HTTP_RESPONSE_MAX] = {};
    int status_code = 0;
    esp_err_t err = ai_home_post_json("/api/v1/dialog", body, response, sizeof(response), &status_code);
    if (err != ESP_OK || status_code < 200 || status_code >= 300) {
        printf("ai_home_dialog err=%s http=%d response=\"%s\"\n", esp_err_to_name(err), status_code, response);
        return;
    }

    char mood[24] = {};
    char reply[512] = {};
    if (json_extract_string(response, "device_mood", mood, sizeof(mood)) && valid_mood(mood)) {
        apply_mood_from_ai(mood);
    }
    if (!json_extract_string(response, "text", reply, sizeof(reply))) {
        snprintf(reply, sizeof(reply), "(no text)");
    }
    printf("ai_home_dialog ok http=%d mood=%s text=\"%s\"\n", status_code, s_state.mood, reply);
}

static void ai_home_task(void* arg) {
    (void)arg;
    while (true) {
        if (s_ai_home_autostart && s_wifi_ready && wifi_is_connected()) {
            ai_home_ping_command(false);
            ai_home_poll_command(false);
        }
        vTaskDelay(pdMS_TO_TICKS(OUO_AI_HEARTBEAT_INTERVAL_MS));
    }
}

static void stop_camera_probe_xclk(void) {
    if (!s_camera_xclk_ready) {
        return;
    }
    ledc_stop(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0, 0);
    gpio_reset_pin(OUO_KORVO_CAMERA_XCLK_GPIO);
    s_camera_xclk_ready = false;
}

static void release_touch_i2c_for_camera(void) {
    cancel_touch_state();
    s_state.touch_ready = false;
    s_state.touch_active = false;
    if (s_touch_dev != NULL) {
        i2c_master_bus_rm_device(s_touch_dev);
        s_touch_dev = NULL;
    }
    if (s_i2c_bus != NULL) {
        i2c_del_master_bus(s_i2c_bus);
        s_i2c_bus = NULL;
    }
}

static bool IRAM_ATTR camera_preview_get_new_buffer(esp_cam_ctlr_handle_t handle, esp_cam_ctlr_trans_t* trans, void* user_data) {
    (void)handle;
    esp_cam_ctlr_trans_t* camera_trans = (esp_cam_ctlr_trans_t*)user_data;
    trans->buffer = camera_trans->buffer;
    trans->buflen = camera_trans->buflen;
    s_camera_preview_new_trans++;
    return false;
}

static bool IRAM_ATTR camera_preview_trans_finished(esp_cam_ctlr_handle_t handle, esp_cam_ctlr_trans_t* trans, void* user_data) {
    (void)handle;
    (void)trans;
    (void)user_data;
    s_camera_preview_finished_trans++;
    return false;
}

static esp_err_t camera_sensor_start(const char* format_name, ouo_camera_sensor_session_t* session) {
    memset(session, 0, sizeof(*session));
    esp_err_t err = ensure_shared_i2c_bus_ready();
    if (err != ESP_OK) {
        return err;
    }

    esp_cam_sensor_config_t cam_config = {
        .reset_pin = OUO_KORVO_CAMERA_RESET_GPIO,
        .pwdn_pin = OUO_KORVO_CAMERA_PWDN_GPIO,
        .xclk_pin = OUO_KORVO_CAMERA_XCLK_GPIO,
    };

    esp_cam_sensor_device_t* cam = NULL;
    esp_sccb_io_handle_t sccb_handle = NULL;
    for (esp_cam_sensor_detect_fn_t* p = &__esp_cam_sensor_detect_fn_array_start;
         p < &__esp_cam_sensor_detect_fn_array_end;
         ++p) {
        sccb_i2c_config_t i2c_config = {
            .scl_speed_hz = OUO_CAMERA_SCCB_FREQ_HZ,
            .device_address = p->sccb_addr,
            .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        };
        err = sccb_new_i2c_io(s_i2c_bus, &i2c_config, &sccb_handle);
        if (err != ESP_OK) {
            return err;
        }

        cam_config.sccb_handle = sccb_handle;
        cam_config.sensor_port = p->port;
        cam = (*(p->detect))(&cam_config);
        if (cam != NULL) {
            if (p->port != ESP_CAM_SENSOR_DVP) {
                esp_sccb_del_i2c_io(sccb_handle);
                return ESP_ERR_NOT_SUPPORTED;
            }
            break;
        }

        esp_sccb_del_i2c_io(sccb_handle);
        sccb_handle = NULL;
    }

    if (cam == NULL) {
        return ESP_ERR_NOT_FOUND;
    }

    esp_cam_sensor_format_array_t format_array = {};
    err = esp_cam_sensor_query_format(cam, &format_array);
    if (err != ESP_OK) {
        esp_sccb_del_i2c_io(sccb_handle);
        return err;
    }

    const esp_cam_sensor_format_t* selected_format = NULL;
    for (int i = 0; i < format_array.count; ++i) {
        ESP_LOGI(TAG, "camera fmt[%d].name:%s", i, format_array.format_array[i].name);
        if (strcmp(format_array.format_array[i].name, format_name) == 0) {
            selected_format = &format_array.format_array[i];
        }
    }
    if (selected_format == NULL) {
        ESP_LOGE(TAG, "camera format unsupported: %s", format_name);
        esp_sccb_del_i2c_io(sccb_handle);
        return ESP_ERR_NOT_SUPPORTED;
    }

    err = esp_cam_sensor_set_format(cam, selected_format);
    if (err != ESP_OK) {
        esp_sccb_del_i2c_io(sccb_handle);
        return err;
    }

    int stream_enable = 1;
    err = esp_cam_sensor_ioctl(cam, ESP_CAM_SENSOR_IOC_S_STREAM, &stream_enable);
    if (err != ESP_OK) {
        esp_sccb_del_i2c_io(sccb_handle);
        return err;
    }

    session->sensor = cam;
    session->sccb_handle = sccb_handle;
    ESP_LOGI(TAG, "camera sensor format in use: %s", format_name);
    return ESP_OK;
}

static void camera_sensor_stop(ouo_camera_sensor_session_t* session) {
    if (session->sensor != NULL) {
        int stream_enable = 0;
        esp_cam_sensor_ioctl(session->sensor, ESP_CAM_SENSOR_IOC_S_STREAM, &stream_enable);
        session->sensor = NULL;
    }
    if (session->sccb_handle != NULL) {
        esp_sccb_del_i2c_io(session->sccb_handle);
        session->sccb_handle = NULL;
    }
}

static void camera_clear_lcd(void) {
    uint16_t* fb = NULL;
    if (esp_lcd_rgb_panel_get_frame_buffer(s_lcd_panel, 1, (void**)&fb) != ESP_OK || fb == NULL) {
        return;
    }
    memset(fb, 0, OUO_KORVO_LCD_H_RES * OUO_KORVO_LCD_V_RES * sizeof(uint16_t));
    esp_cache_msync(fb,
                    OUO_KORVO_LCD_H_RES * OUO_KORVO_LCD_V_RES * sizeof(uint16_t),
                    ESP_CACHE_MSYNC_FLAG_DIR_C2M);
    esp_lcd_panel_draw_bitmap(s_lcd_panel, 0, 0, OUO_KORVO_LCD_H_RES, OUO_KORVO_LCD_V_RES, fb);
}

static void camera_swap_rgb565_be_to_lcd(const uint8_t* src, uint8_t* dst, size_t byte_count) {
    for (size_t i = 0; i + 1 < byte_count; i += 2) {
        dst[i] = src[i + 1];
        dst[i + 1] = src[i];
    }
}

static void put_le16(uint8_t* out, uint16_t value) {
    out[0] = (uint8_t)(value & 0xff);
    out[1] = (uint8_t)((value >> 8) & 0xff);
}

static void put_le32(uint8_t* out, uint32_t value) {
    out[0] = (uint8_t)(value & 0xff);
    out[1] = (uint8_t)((value >> 8) & 0xff);
    out[2] = (uint8_t)((value >> 16) & 0xff);
    out[3] = (uint8_t)((value >> 24) & 0xff);
}

static esp_err_t camera_rgb565_be_to_bmp24(const uint8_t* src, int width, int height, uint8_t** out_bmp, size_t* out_size) {
    if (src == NULL || out_bmp == NULL || out_size == NULL || width <= 0 || height <= 0) {
        return ESP_ERR_INVALID_ARG;
    }

    const size_t header_size = 54;
    const size_t row_stride = ((size_t)width * 3U + 3U) & ~3U;
    const size_t pixel_bytes = row_stride * (size_t)height;
    const size_t bmp_size = header_size + pixel_bytes;
    uint8_t* bmp = heap_caps_calloc(1, bmp_size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (bmp == NULL) {
        return ESP_ERR_NO_MEM;
    }

    bmp[0] = 'B';
    bmp[1] = 'M';
    put_le32(&bmp[2], (uint32_t)bmp_size);
    put_le32(&bmp[10], (uint32_t)header_size);
    put_le32(&bmp[14], 40);
    put_le32(&bmp[18], (uint32_t)width);
    put_le32(&bmp[22], (uint32_t)(-height));
    put_le16(&bmp[26], 1);
    put_le16(&bmp[28], 24);
    put_le32(&bmp[34], (uint32_t)pixel_bytes);

    for (int y = 0; y < height; ++y) {
        uint8_t* row = bmp + header_size + (size_t)y * row_stride;
        for (int x = 0; x < width; ++x) {
            const size_t src_i = ((size_t)y * (size_t)width + (size_t)x) * OUO_CAMERA_RGB565_BYTES_PER_PIXEL;
            const uint16_t rgb565 = ((uint16_t)src[src_i] << 8) | src[src_i + 1];
            const uint8_t r = (uint8_t)((((rgb565 >> 11) & 0x1f) * 255U) / 31U);
            const uint8_t g = (uint8_t)((((rgb565 >> 5) & 0x3f) * 255U) / 63U);
            const uint8_t b = (uint8_t)(((rgb565 & 0x1f) * 255U) / 31U);
            row[(size_t)x * 3U + 0U] = b;
            row[(size_t)x * 3U + 1U] = g;
            row[(size_t)x * 3U + 2U] = r;
        }
    }

    *out_bmp = bmp;
    *out_size = bmp_size;
    return ESP_OK;
}

static esp_err_t camera_capture_rgb565_be(const ouo_camera_preview_format_t* format, uint8_t** out_frame, size_t* out_size) {
    if (format == NULL || out_frame == NULL || out_size == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    esp_cam_ctlr_handle_t cam_handle = NULL;
    void* cam_buffer = NULL;
    ouo_camera_sensor_session_t sensor_session = {};
    esp_err_t result = ESP_OK;

    esp_cam_ctlr_dvp_pin_config_t pin_cfg = {
        .data_width = OUO_KORVO_CAMERA_DATA_WIDTH,
        .data_io = {
            OUO_KORVO_CAMERA_D0_GPIO,
            OUO_KORVO_CAMERA_D1_GPIO,
            OUO_KORVO_CAMERA_D2_GPIO,
            OUO_KORVO_CAMERA_D3_GPIO,
            OUO_KORVO_CAMERA_D4_GPIO,
            OUO_KORVO_CAMERA_D5_GPIO,
            OUO_KORVO_CAMERA_D6_GPIO,
            OUO_KORVO_CAMERA_D7_GPIO,
        },
        .vsync_io = OUO_KORVO_CAMERA_VSYNC_GPIO,
        .de_io = OUO_KORVO_CAMERA_DE_GPIO,
        .pclk_io = OUO_KORVO_CAMERA_PCLK_GPIO,
        .xclk_io = OUO_KORVO_CAMERA_XCLK_GPIO,
    };

    esp_cam_ctlr_dvp_config_t dvp_config = {
        .ctlr_id = 0,
        .clk_src = CAM_CLK_SRC_DEFAULT,
        .h_res = format->width,
        .v_res = format->height,
        .input_data_color_type = CAM_CTLR_COLOR_RGB565,
        .output_data_color_type = CAM_CTLR_COLOR_RGB565,
        .dma_burst_size = 64,
        .pin = &pin_cfg,
        .bk_buffer_dis = 1,
        .xclk_freq = OUO_KORVO_CAMERA_XCLK_HZ,
        .cam_data_width = OUO_KORVO_CAMERA_DATA_WIDTH,
        .bit_swap_en = false,
        .byte_swap_en = false,
    };

    result = esp_cam_new_dvp_ctlr(&dvp_config, &cam_handle);
    if (result != ESP_OK) {
        goto cleanup;
    }

    const size_t frame_size = (size_t)format->width * (size_t)format->height * OUO_CAMERA_RGB565_BYTES_PER_PIXEL;
    cam_buffer = esp_cam_ctlr_alloc_buffer(cam_handle, frame_size, MALLOC_CAP_DMA | MALLOC_CAP_SPIRAM);
    if (cam_buffer == NULL) {
        result = ESP_ERR_NO_MEM;
        goto cleanup;
    }

    result = camera_sensor_start(format->name, &sensor_session);
    if (result != ESP_OK) {
        goto cleanup;
    }

    esp_cam_ctlr_trans_t trans_data = {
        .buffer = cam_buffer,
        .buflen = frame_size,
    };
    esp_cam_ctlr_evt_cbs_t cbs = {
        .on_get_new_trans = camera_preview_get_new_buffer,
        .on_trans_finished = camera_preview_trans_finished,
    };
    result = esp_cam_ctlr_register_event_callbacks(cam_handle, &cbs, &trans_data);
    if (result != ESP_OK) {
        goto cleanup;
    }

    const uint32_t start_finished = (uint32_t)s_camera_preview_finished_trans;
    result = esp_cam_ctlr_enable(cam_handle);
    if (result != ESP_OK) {
        goto cleanup;
    }
    result = esp_cam_ctlr_start(cam_handle);
    if (result != ESP_OK) {
        goto cleanup;
    }

    result = ESP_ERR_TIMEOUT;
    for (int i = 0; i < 50; ++i) {
        if ((uint32_t)s_camera_preview_finished_trans > start_finished) {
            esp_cache_msync(cam_buffer, frame_size, ESP_CACHE_MSYNC_FLAG_DIR_M2C);
            uint8_t* frame_copy = heap_caps_malloc(frame_size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
            if (frame_copy == NULL) {
                result = ESP_ERR_NO_MEM;
                goto cleanup;
            }
            memcpy(frame_copy, cam_buffer, frame_size);
            *out_frame = frame_copy;
            *out_size = frame_size;
            result = ESP_OK;
            break;
        }
        vTaskDelay(pdMS_TO_TICKS(100));
    }

cleanup:
    if (cam_handle != NULL) {
        esp_cam_ctlr_stop(cam_handle);
        esp_cam_ctlr_disable(cam_handle);
    }
    camera_sensor_stop(&sensor_session);
    if (cam_handle != NULL) {
        esp_cam_ctlr_del(cam_handle);
    }
    if (cam_buffer != NULL) {
        heap_caps_free(cam_buffer);
    }
    release_touch_i2c_for_camera();
    return result;
}

static void ai_home_camera_snapshot_command(void) {
    if (s_camera_preview_active) {
        printf("ai_home_camera_snapshot err=camera_busy\n");
        return;
    }

    static const ouo_camera_preview_format_t format = {
        "DVP_8bit_20Minput_RGB565_BE_240x240_24fps",
        240,
        240,
    };

    stop_camera_probe_xclk();
    vTaskDelay(pdMS_TO_TICKS(100));
    release_touch_i2c_for_camera();
    printf("ai_home_camera_snapshot begin format=\"%s\"\n", format.name);

    uint8_t* frame = NULL;
    size_t frame_size = 0;
    esp_err_t err = ESP_FAIL;
    for (int attempt = 1; attempt <= 2; ++attempt) {
        s_camera_preview_new_trans = 0;
        s_camera_preview_finished_trans = 0;
        err = camera_capture_rgb565_be(&format, &frame, &frame_size);
        if (err == ESP_OK) {
            break;
        }
        printf("ai_home_camera_snapshot capture_attempt=%d err=%s new_trans=%" PRIu32 " finished=%" PRIu32 "\n",
               attempt,
               esp_err_to_name(err),
               (uint32_t)s_camera_preview_new_trans,
               (uint32_t)s_camera_preview_finished_trans);
        vTaskDelay(pdMS_TO_TICKS(300));
    }
    if (err != ESP_OK) {
        printf("ai_home_camera_snapshot err=%s step=capture\n", esp_err_to_name(err));
        return;
    }

    uint8_t* bmp = NULL;
    size_t bmp_size = 0;
    err = camera_rgb565_be_to_bmp24(frame, format.width, format.height, &bmp, &bmp_size);
    heap_caps_free(frame);
    if (err != ESP_OK) {
        printf("ai_home_camera_snapshot err=%s step=bmp\n", esp_err_to_name(err));
        return;
    }

    const size_t b64_cap = ((bmp_size + 2U) / 3U) * 4U + 1U;
    unsigned char* b64 = heap_caps_malloc(b64_cap, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (b64 == NULL) {
        heap_caps_free(bmp);
        printf("ai_home_camera_snapshot err=no_memory step=base64_alloc size=%u\n", (unsigned)b64_cap);
        return;
    }

    size_t b64_len = 0;
    int enc = mbedtls_base64_encode(b64, b64_cap, &b64_len, bmp, bmp_size);
    heap_caps_free(bmp);
    if (enc != 0) {
        heap_caps_free(b64);
        printf("ai_home_camera_snapshot err=base64_%d\n", enc);
        return;
    }
    b64[b64_len] = '\0';

    const size_t body_cap = b64_len + 256U;
    char* body = heap_caps_malloc(body_cap, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (body == NULL) {
        heap_caps_free(b64);
        printf("ai_home_camera_snapshot err=no_memory step=body_alloc size=%u\n", (unsigned)body_cap);
        return;
    }
    snprintf(body,
             body_cap,
             "{\"device_id\":\"ouo-s31-korvo-1\",\"mime\":\"image/bmp\",\"width\":%d,\"height\":%d,\"image_base64\":\"%s\"}",
             format.width,
             format.height,
             (const char*)b64);
    heap_caps_free(b64);

    char response[OUO_AI_HTTP_RESPONSE_MAX] = {};
    int status_code = 0;
    err = ai_home_post_json_timeout("/api/v1/camera/frame",
                                    body,
                                    response,
                                    sizeof(response),
                                    &status_code,
                                    OUO_AI_CAMERA_HTTP_TIMEOUT_MS);
    heap_caps_free(body);
    if (err != ESP_OK || status_code < 200 || status_code >= 300) {
        printf("ai_home_camera_snapshot err=%s http=%d response=\"%s\"\n", esp_err_to_name(err), status_code, response);
        return;
    }

    printf("ai_home_camera_snapshot ok http=%d width=%d height=%d bytes=%u response=\"%s\"\n",
           status_code,
           format.width,
           format.height,
           (unsigned)bmp_size,
           response);
}

static esp_err_t camera_preview_run_format(const ouo_camera_preview_format_t* format, int seconds) {
    esp_cam_ctlr_handle_t cam_handle = NULL;
    void* cam_buffer = NULL;
    void* display_buffer = NULL;
    ouo_camera_sensor_session_t sensor_session = {};
    esp_err_t result = ESP_OK;

    esp_cam_ctlr_dvp_pin_config_t pin_cfg = {
        .data_width = OUO_KORVO_CAMERA_DATA_WIDTH,
        .data_io = {
            OUO_KORVO_CAMERA_D0_GPIO,
            OUO_KORVO_CAMERA_D1_GPIO,
            OUO_KORVO_CAMERA_D2_GPIO,
            OUO_KORVO_CAMERA_D3_GPIO,
            OUO_KORVO_CAMERA_D4_GPIO,
            OUO_KORVO_CAMERA_D5_GPIO,
            OUO_KORVO_CAMERA_D6_GPIO,
            OUO_KORVO_CAMERA_D7_GPIO,
        },
        .vsync_io = OUO_KORVO_CAMERA_VSYNC_GPIO,
        .de_io = OUO_KORVO_CAMERA_DE_GPIO,
        .pclk_io = OUO_KORVO_CAMERA_PCLK_GPIO,
        .xclk_io = OUO_KORVO_CAMERA_XCLK_GPIO,
    };

    esp_cam_ctlr_dvp_config_t dvp_config = {
        .ctlr_id = 0,
        .clk_src = CAM_CLK_SRC_DEFAULT,
        .h_res = format->width,
        .v_res = format->height,
        .input_data_color_type = CAM_CTLR_COLOR_RGB565,
        .output_data_color_type = CAM_CTLR_COLOR_RGB565,
        .dma_burst_size = 64,
        .pin = &pin_cfg,
        .bk_buffer_dis = 1,
        .xclk_freq = OUO_KORVO_CAMERA_XCLK_HZ,
        .cam_data_width = OUO_KORVO_CAMERA_DATA_WIDTH,
        .bit_swap_en = false,
        .byte_swap_en = false,
    };

    result = esp_cam_new_dvp_ctlr(&dvp_config, &cam_handle);
    if (result != ESP_OK) {
        printf("camera_preview err=%s step=dvp_init format=\"%s\"\n", esp_err_to_name(result), format->name);
        goto cleanup;
    }

    size_t cam_buffer_size = (size_t)format->width * (size_t)format->height * OUO_CAMERA_RGB565_BYTES_PER_PIXEL;
    cam_buffer = esp_cam_ctlr_alloc_buffer(cam_handle, cam_buffer_size, MALLOC_CAP_DMA | MALLOC_CAP_SPIRAM);
    if (cam_buffer == NULL) {
        result = ESP_ERR_NO_MEM;
        printf("camera_preview err=no_memory step=alloc size=%u\n", (unsigned)cam_buffer_size);
        goto cleanup;
    }
    display_buffer = heap_caps_malloc(cam_buffer_size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (display_buffer == NULL) {
        result = ESP_ERR_NO_MEM;
        printf("camera_preview err=no_memory step=display_alloc size=%u\n", (unsigned)cam_buffer_size);
        goto cleanup;
    }

    result = camera_sensor_start(format->name, &sensor_session);
    if (result != ESP_OK) {
        printf("camera_preview err=%s step=sensor format=\"%s\"\n", esp_err_to_name(result), format->name);
        goto cleanup;
    }

    esp_cam_ctlr_trans_t trans_data = {
        .buffer = cam_buffer,
        .buflen = cam_buffer_size,
    };
    esp_cam_ctlr_evt_cbs_t cbs = {
        .on_get_new_trans = camera_preview_get_new_buffer,
        .on_trans_finished = camera_preview_trans_finished,
    };
    result = esp_cam_ctlr_register_event_callbacks(cam_handle, &cbs, &trans_data);
    if (result != ESP_OK) {
        printf("camera_preview err=%s step=callbacks\n", esp_err_to_name(result));
        goto cleanup;
    }

    result = esp_cam_ctlr_enable(cam_handle);
    if (result != ESP_OK) {
        printf("camera_preview err=%s step=enable\n", esp_err_to_name(result));
        goto cleanup;
    }

    result = esp_cam_ctlr_start(cam_handle);
    if (result != ESP_OK) {
        printf("camera_preview err=%s step=start\n", esp_err_to_name(result));
        goto cleanup;
    }

    const int x0 = (OUO_KORVO_LCD_H_RES - format->width) / 2;
    const int y0 = (OUO_KORVO_LCD_V_RES - format->height) / 2;
    const uint32_t end_ms = (uint32_t)(esp_timer_get_time() / 1000) + (uint32_t)seconds * 1000U;
    uint32_t draws = 0;
    printf("camera_preview streaming format=\"%s\" x=%d y=%d seconds=%d\n", format->name, x0, y0, seconds);
    while ((int32_t)(end_ms - (uint32_t)(esp_timer_get_time() / 1000)) > 0) {
        esp_cache_msync(cam_buffer, cam_buffer_size, ESP_CACHE_MSYNC_FLAG_DIR_M2C);
        camera_swap_rgb565_be_to_lcd((const uint8_t*)cam_buffer, (uint8_t*)display_buffer, cam_buffer_size);
        esp_cache_msync(display_buffer, cam_buffer_size, ESP_CACHE_MSYNC_FLAG_DIR_C2M | ESP_CACHE_MSYNC_FLAG_UNALIGNED);
        esp_lcd_panel_draw_bitmap(s_lcd_panel, x0, y0, x0 + format->width, y0 + format->height, display_buffer);
        draws++;
        vTaskDelay(pdMS_TO_TICKS(40));
    }
    printf("camera_preview ok format=\"%s\" draws=%" PRIu32 " new_trans=%" PRIu32 " finished=%" PRIu32 "\n",
           format->name,
           draws,
           (uint32_t)s_camera_preview_new_trans,
           (uint32_t)s_camera_preview_finished_trans);

cleanup:
    if (cam_handle != NULL) {
        esp_cam_ctlr_stop(cam_handle);
        esp_cam_ctlr_disable(cam_handle);
    }
    camera_sensor_stop(&sensor_session);
    if (cam_handle != NULL) {
        esp_cam_ctlr_del(cam_handle);
    }
    if (cam_buffer != NULL) {
        heap_caps_free(cam_buffer);
    }
    if (display_buffer != NULL) {
        heap_caps_free(display_buffer);
    }
    release_touch_i2c_for_camera();
    return result;
}

static void camera_preview_command(const char* args) {
    if (s_camera_preview_active) {
        printf("camera_preview err=already_active\n");
        return;
    }

    int seconds = OUO_CAMERA_DEFAULT_PREVIEW_SECONDS;
    if (args != NULL && args[0] != '\0') {
        char* end = NULL;
        long parsed = strtol(args, &end, 10);
        if (end == args || (*end != '\0' && !isspace((unsigned char)*end)) || parsed <= 0 || parsed > OUO_CAMERA_MAX_PREVIEW_SECONDS) {
            printf("err use: camera_preview [1-%d seconds]\n", OUO_CAMERA_MAX_PREVIEW_SECONDS);
            return;
        }
        seconds = (int)parsed;
    }

    esp_err_t err = ensure_korvo_lcd_ready();
    if (err != ESP_OK) {
        printf("camera_preview err=%s step=lcd\n", esp_err_to_name(err));
        return;
    }

    static const ouo_camera_preview_format_t formats[] = {
        {"DVP_8bit_20Minput_RGB565_BE_240x240_24fps", 240, 240},
        {"DVP_8bit_20Minput_RGB565_BE_640x480_10fps", 640, 480},
    };

    s_camera_preview_active = true;
    s_state.display_mode = "camera_preview";
    close_touch_menu();
    cancel_touch_state();
    stop_camera_probe_xclk();
    vTaskDelay(pdMS_TO_TICKS(100));
    release_touch_i2c_for_camera();
    camera_clear_lcd();

    s_camera_preview_new_trans = 0;
    s_camera_preview_finished_trans = 0;
    printf("camera_preview begin profile=korvo_1 sensor=OV3660 seconds=%d\n", seconds);

    esp_err_t last_err = ESP_FAIL;
    for (size_t i = 0; i < sizeof(formats) / sizeof(formats[0]); ++i) {
        last_err = camera_preview_run_format(&formats[i], seconds);
        if (last_err == ESP_OK) {
            break;
        }
    }

    s_state.display_mode = "ouo_runtime_rgb";
    s_camera_preview_active = false;
    if (last_err != ESP_OK) {
        printf("camera_preview failed last_err=%s\n", esp_err_to_name(last_err));
    }
}

static esp_err_t ensure_camera_xclk_ready(void) {
    if (s_camera_xclk_ready) {
        return ESP_OK;
    }
    ledc_timer_config_t timer_config = {
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .duty_resolution = LEDC_TIMER_1_BIT,
        .timer_num = LEDC_TIMER_0,
        .freq_hz = OUO_KORVO_CAMERA_XCLK_HZ,
        .clk_cfg = LEDC_AUTO_CLK,
    };
    esp_err_t err = ledc_timer_config(&timer_config);
    if (err != ESP_OK) {
        return err;
    }
    ledc_channel_config_t channel_config = {
        .gpio_num = OUO_KORVO_CAMERA_XCLK_GPIO,
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .channel = LEDC_CHANNEL_0,
        .intr_type = LEDC_INTR_DISABLE,
        .timer_sel = LEDC_TIMER_0,
        .duty = 1,
        .hpoint = 0,
    };
    err = ledc_channel_config(&channel_config);
    if (err == ESP_OK) {
        s_camera_xclk_ready = true;
    }
    return err;
}

static esp_err_t camera_sccb_read_reg(uint8_t addr, uint16_t reg, uint8_t* value) {
    i2c_master_dev_handle_t dev = NULL;
    i2c_device_config_t dev_config = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = addr,
        .scl_speed_hz = 100000,
    };
    esp_err_t err = i2c_master_bus_add_device(s_i2c_bus, &dev_config, &dev);
    if (err != ESP_OK) {
        return err;
    }
    uint8_t reg_buf[2] = {(uint8_t)(reg >> 8), (uint8_t)(reg & 0xff)};
    err = i2c_master_transmit_receive(dev, reg_buf, sizeof(reg_buf), value, 1, 100);
    i2c_master_bus_rm_device(dev);
    return err;
}

static void camera_probe_command(void) {
    printf("camera_probe begin interface=dvp sensor_hint=OV3660 xclk_gpio=%d reset_gpio=%d pwdn_gpio=%d\n",
           OUO_KORVO_CAMERA_XCLK_GPIO,
           OUO_KORVO_CAMERA_RESET_GPIO,
           OUO_KORVO_CAMERA_PWDN_GPIO);

    esp_err_t err = ensure_shared_i2c_bus_ready();
    if (err != ESP_OK) {
        printf("camera_probe i2c_err=%s\n", esp_err_to_name(err));
        return;
    }
    err = ensure_camera_xclk_ready();
    if (err != ESP_OK) {
        printf("camera_probe xclk_err=%s\n", esp_err_to_name(err));
        return;
    }

    gpio_config_t gpio_cfg = {
        .pin_bit_mask = (1ULL << OUO_KORVO_CAMERA_RESET_GPIO) | (1ULL << OUO_KORVO_CAMERA_PWDN_GPIO),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&gpio_cfg);
    gpio_set_level(OUO_KORVO_CAMERA_PWDN_GPIO, 0);
    gpio_set_level(OUO_KORVO_CAMERA_RESET_GPIO, 0);
    vTaskDelay(pdMS_TO_TICKS(10));
    gpio_set_level(OUO_KORVO_CAMERA_RESET_GPIO, 1);
    vTaskDelay(pdMS_TO_TICKS(50));

    const uint8_t candidates[] = {0x30, 0x3c, 0x1e, 0x21, 0x36};
    printf("camera_probe candidate_ack=");
    int found = 0;
    for (size_t i = 0; i < sizeof(candidates); ++i) {
        if (i2c_master_probe(s_i2c_bus, candidates[i], 20) == ESP_OK) {
            printf("%s0x%02x", found == 0 ? "" : ",", candidates[i]);
            found++;
        }
    }
    if (found == 0) {
        printf("none");
    }
    printf("\n");

    for (size_t i = 0; i < sizeof(candidates); ++i) {
        uint8_t high = 0;
        uint8_t low = 0;
        esp_err_t high_err = camera_sccb_read_reg(candidates[i], 0x300a, &high);
        esp_err_t low_err = camera_sccb_read_reg(candidates[i], 0x300b, &low);
        if (high_err == ESP_OK && low_err == ESP_OK) {
            printf("camera_probe ov_chip_id addr=0x%02x id=0x%02x%02x\n", candidates[i], high, low);
        }
    }
    printf("camera_focus=fixed_or_manual_lens autofocus_api=not_detected\n");
}

static void camera_focus_command(void) {
    printf("camera_focus unsupported: Korvo1 accessory is documented as OV3660 module; this path exposes fixed/manual optical focus, not an autofocus motor API.\n");
}

static void process_command(char* line) {
    trim_line(line);
    char original[256] = {};
    snprintf(original, sizeof(original), "%s", line);
    lower_line(line);
    if (line[0] == '\0') {
        return;
    }
    s_state.commands++;

    if (strcmp(line, "help") == 0) {
        print_help();
        return;
    }
    if (strcmp(line, "status") == 0 || strcmp(line, "diag") == 0) {
        print_status();
        return;
    }
    if (strcmp(line, "mac") == 0) {
        print_mac_info();
        return;
    }
    if (strcmp(line, "partitions") == 0 || strcmp(line, "partition") == 0) {
        print_partition_info();
        return;
    }
    if (strcmp(line, "ota_status") == 0 || strcmp(line, "ota") == 0) {
        ota_status_command();
        return;
    }
    if (strcmp(line, "ota_check") == 0) {
        ota_check_command();
        return;
    }
    if (strcmp(line, "ota_update") == 0) {
        ota_update_command();
        return;
    }
    if (strncmp(line, "ota_manifest_url ", 17) == 0) {
        ota_manifest_url_command(original + 17);
        return;
    }
    if (strcmp(line, "storage_test") == 0 || strcmp(line, "storage") == 0) {
        storage_test_command();
        return;
    }
    if (strcmp(line, "ai_home_status") == 0 || strcmp(line, "ai_home") == 0) {
        ai_home_status_command();
        return;
    }
    if (strncmp(line, "ai_home_server ", 15) == 0) {
        ai_home_server_command(original + 15);
        return;
    }
    if (strcmp(line, "ai_home_ping") == 0) {
        ai_home_ping_command(true);
        return;
    }
    if (strcmp(line, "ai_home_poll") == 0) {
        ai_home_poll_command(true);
        return;
    }
    if (strncmp(line, "ai_home_dialog ", 15) == 0) {
        ai_home_dialog_command(original + 15);
        return;
    }
    if (strcmp(line, "ai_home_camera_snapshot") == 0) {
        ai_home_camera_snapshot_command();
        return;
    }
    if (strncmp(line, "ai_home_autostart ", 18) == 0) {
        ai_home_autostart_command(line + 18);
        return;
    }
    if (strcmp(line, "wake") == 0) {
        wake_command("");
        return;
    }
    if (strncmp(line, "wake ", 5) == 0) {
        wake_command(original + 5);
        return;
    }
    if (strncmp(line, "emotion_map ", 12) == 0) {
        emotion_map_command(original + 12);
        return;
    }
    if (strcmp(line, "board_info") == 0 || strcmp(line, "board") == 0) {
        print_board_info();
        return;
    }
    if (strcmp(line, "lcd_probe") == 0 || strcmp(line, "probe") == 0) {
        lcd_probe_command();
        return;
    }
    if (strcmp(line, "lcd_test") == 0 || strcmp(line, "lcd") == 0) {
        lcd_test_command();
        return;
    }
    if (strcmp(line, "renderer_test") == 0 || strcmp(line, "renderer") == 0 || strcmp(line, "ouo") == 0) {
        renderer_test_command();
        return;
    }
    if (strcmp(line, "touch_status") == 0 || strcmp(line, "touch") == 0) {
        print_touch_status();
        return;
    }
    if (strcmp(line, "korvo_led_test") == 0 || strcmp(line, "led_test") == 0) {
        korvo_led_test_command(OUO_KORVO_RGB_LED_GPIO);
        return;
    }
    if (strncmp(line, "korvo_led_test ", 15) == 0) {
        char* end = NULL;
        long gpio_num = strtol(line + 15, &end, 10);
        if (end == line + 15 || *end != '\0' || gpio_num < 0 || gpio_num > 61) {
            printf("err use: korvo_led_test [gpio]\n");
            return;
        }
        korvo_led_test_command((int)gpio_num);
        return;
    }
    if (strcmp(line, "function_led_test") == 0) {
        function_led_test_command();
        return;
    }
    if (strcmp(line, "wifi_scan") == 0 || strcmp(line, "scan") == 0) {
        wifi_scan_command();
        return;
    }
    if (strcmp(line, "wifi_status") == 0 || strcmp(line, "wifi") == 0) {
        wifi_status_command();
        return;
    }
    if (strncmp(line, "wifi_autoconnect ", 17) == 0) {
        wifi_autoconnect_command(line + 17);
        return;
    }
    if (strcmp(line, "wifi_forget") == 0) {
        wifi_forget_command();
        return;
    }
    if (strncmp(line, "wifi_connect ", 13) == 0) {
        wifi_connect_command(original + 13);
        return;
    }
    if (strcmp(line, "camera_probe") == 0 || strcmp(line, "camera") == 0) {
        camera_probe_command();
        return;
    }
    if (strcmp(line, "camera_preview") == 0) {
        camera_preview_command("");
        return;
    }
    if (strncmp(line, "camera_preview ", 15) == 0) {
        camera_preview_command(line + 15);
        return;
    }
    if (strcmp(line, "camera_focus") == 0 || strncmp(line, "camera_focus ", 13) == 0) {
        camera_focus_command();
        return;
    }
    if (strcmp(line, "reboot") == 0) {
        printf("rebooting\n");
        fflush(stdout);
        vTaskDelay(pdMS_TO_TICKS(50));
        esp_restart();
    }
    if (strncmp(line, "mood ", 5) == 0) {
        const char* mood = line + 5;
        if (!valid_mood(mood)) {
            printf("err unknown mood\n");
            return;
        }
        if (!set_current_mood(mood)) {
            printf("err renderer mood\n");
            return;
        }
        printf("ok mood=%s\n", s_state.mood);
        return;
    }
    if (strncmp(line, "blush ", 6) == 0) {
        bool enabled = false;
        if (!parse_on_off(line + 6, &enabled)) {
            printf("err use: blush on|off\n");
            return;
        }
        s_state.blush = enabled;
        if (s_state.renderer_ready) {
            ouo_s31_renderer_set_blush(enabled);
        }
        printf("ok blush=%d\n", enabled);
        return;
    }
    if (strncmp(line, "option ", 7) == 0) {
        char* value = strchr(line + 7, ' ');
        if (value == NULL) {
            printf("err use: option lock|static|stretch|tilt on|off\n");
            return;
        }
        *value++ = '\0';
        bool enabled = false;
        if (!parse_on_off(value, &enabled)) {
            printf("err option value on|off\n");
            return;
        }
        const char* key = line + 7;
        if (strcmp(key, "lock") == 0) {
            s_state.lock = enabled;
        } else if (strcmp(key, "static") == 0) {
            s_state.static_mood = enabled;
        } else if (strcmp(key, "stretch") == 0) {
            s_state.stretch = enabled;
        } else if (strcmp(key, "tilt") == 0) {
            s_state.tilt = enabled;
        } else {
            printf("err unknown option\n");
            return;
        }
        if (s_state.renderer_ready && !ouo_s31_renderer_set_option(key, enabled)) {
            printf("err renderer option\n");
            return;
        }
        printf("ok option %s=%d\n", key, enabled);
        return;
    }
    print_help();
}

static void render_task(void* arg) {
    (void)arg;
    while (true) {
        if (s_state.renderer_ready && !s_camera_preview_active) {
            ouo_s31_renderer_tick((uint32_t)(esp_timer_get_time() / 1000));
        }
        vTaskDelay(pdMS_TO_TICKS(16));
    }
}

static uint16_t clamp_touch_coord(uint16_t value, uint16_t max_value) {
    return value > max_value ? max_value : value;
}

static void touch_task(void* arg) {
    (void)arg;
    bool logged_init_error = false;
    uint32_t last_sample_ms = 0;

    while (true) {
        if (s_camera_preview_active) {
            vTaskDelay(pdMS_TO_TICKS(100));
            continue;
        }

        if (!s_state.touch_ready) {
            esp_err_t err = ensure_touch_ready();
            if (err != ESP_OK) {
                s_state.touch_errors++;
                if (!logged_init_error) {
                    ESP_LOGW(TAG, "GT1151 touch init failed: %s", esp_err_to_name(err));
                    logged_init_error = true;
                }
                vTaskDelay(pdMS_TO_TICKS(2000));
                continue;
            }
            ESP_LOGI(TAG, "GT1151 touch ready i2c_addr=0x%02x sda=%d scl=%d",
                     OUO_GT1151_I2C_ADDR,
                     OUO_KORVO_TOUCH_SDA_GPIO,
                     OUO_KORVO_TOUCH_SCL_GPIO);
        }

        if (!s_state.renderer_ready) {
            vTaskDelay(pdMS_TO_TICKS(50));
            continue;
        }

        uint8_t status = 0;
        esp_err_t err = gt1151_read_reg(OUO_GT1151_STATUS_REG, &status, sizeof(status));
        uint32_t now_ms = (uint32_t)(esp_timer_get_time() / 1000);
        if (err != ESP_OK) {
            s_state.touch_errors++;
            if (s_state.touch_active && now_ms - last_sample_ms > OUO_TOUCH_STALE_CANCEL_MS) {
                cancel_touch_state();
            }
            vTaskDelay(pdMS_TO_TICKS(30));
            continue;
        }

        if ((status & 0x80) == 0) {
            if (s_state.touch_active) {
                process_touch_sample(true, s_state.touch_x, s_state.touch_y, now_ms);
                if (now_ms - last_sample_ms > OUO_TOUCH_STALE_CANCEL_MS) {
                    cancel_touch_state();
                }
            }
            vTaskDelay(pdMS_TO_TICKS(30));
            continue;
        }

        uint8_t point_count = status & 0x0f;
        if (point_count > 0 && point_count <= 5) {
            uint8_t point[8] = {};
            err = gt1151_read_reg(OUO_GT1151_POINT_REG, point, sizeof(point));
            if (err == ESP_OK) {
                uint16_t x = (uint16_t)(point[1] | ((uint16_t)point[2] << 8));
                uint16_t y = (uint16_t)(point[3] | ((uint16_t)point[4] << 8));
                x = clamp_touch_coord(x, OUO_KORVO_LCD_H_RES - 1);
                y = clamp_touch_coord(y, OUO_KORVO_LCD_V_RES - 1);
                s_state.touch_events++;
                last_sample_ms = now_ms;
                process_touch_sample(true, x, y, now_ms);
            } else {
                s_state.touch_errors++;
            }
        } else if (s_state.touch_active) {
            s_state.touch_events++;
            process_touch_sample(false, s_state.touch_x, s_state.touch_y, now_ms);
        }

        gt1151_write_u8(OUO_GT1151_STATUS_REG, 0);
        vTaskDelay(pdMS_TO_TICKS(30));
    }
}

static void load_boot_count(void) {
    nvs_handle_t nvs = 0;
    esp_err_t err = nvs_open("ouo", NVS_READWRITE, &nvs);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "nvs_open failed: %s", esp_err_to_name(err));
        return;
    }

    uint32_t count = 0;
    err = nvs_get_u32(nvs, "boots", &count);
    if (err != ESP_OK && err != ESP_ERR_NVS_NOT_FOUND) {
        ESP_LOGW(TAG, "nvs_get_u32 boots failed: %s", esp_err_to_name(err));
    }

    s_state.boot_count = count + 1;
    err = nvs_set_u32(nvs, "boots", s_state.boot_count);
    if (err == ESP_OK) {
        err = nvs_commit(nvs);
    }
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "nvs boot count commit failed: %s", esp_err_to_name(err));
    }

    nvs_close(nvs);
}

static void serial_task(void* arg) {
    (void)arg;
    char line[256] = {};
    size_t len = 0;
    uint8_t byte = 0;

    while (true) {
        int read = uart_read_bytes(UART_NUM_0, &byte, 1, pdMS_TO_TICKS(50));
        if (read <= 0) {
            continue;
        }
        if (byte == '\r') {
            continue;
        }
        if (byte == '\n') {
            line[len] = '\0';
            process_command(line);
            len = 0;
            continue;
        }
        if (len + 1 < sizeof(line)) {
            line[len++] = (char)byte;
        }
    }
}

void app_main(void) {
    esp_err_t nvs_err = nvs_flash_init();
    if (nvs_err == ESP_ERR_NVS_NO_FREE_PAGES || nvs_err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        nvs_err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(nvs_err);

    const uart_config_t uart_config = {
        .baud_rate = 115200,
        .data_bits = UART_DATA_8_BITS,
        .parity = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };
    ESP_ERROR_CHECK(uart_param_config(UART_NUM_0, &uart_config));
    ESP_ERROR_CHECK(uart_driver_install(UART_NUM_0, 2048, 0, 0, NULL, 0));

    s_state.boot_ms = (uint32_t)(esp_timer_get_time() / 1000);
    load_boot_count();
    load_ai_home_config();
    load_wifi_config();
    wifi_autoconnect_start();
    ESP_LOGI(TAG, "OuO ESP32-S31 bring-up started");
    ESP_LOGI(TAG, "serial commands: help, status, diag, mac, partitions, ota_status, ota_check, ota_update, ai_home_status, ai_home_server <url>, ai_home_ping, ai_home_poll, ai_home_dialog <text>, ai_home_camera_snapshot, ai_home_autostart on|off, wake <phrase> [confidence], emotion_map <text>, board_info, storage_test, wifi_scan, wifi_status, wifi_connect <ssid> <password>, wifi_autoconnect on|off, wifi_forget, camera_probe, camera_preview [seconds], camera_focus, lcd_probe, lcd_test, renderer_test, touch_status, korvo_led_test [gpio], function_led_test, mood <name>, blush on|off, option <key> on|off, reboot");
    renderer_test_command();
    print_status();

    xTaskCreate(serial_task, "ouo_serial", 8192, NULL, 5, NULL);
    xTaskCreate(render_task, "ouo_render", 8192, NULL, 5, &s_render_task_handle);
    xTaskCreate(touch_task, "ouo_touch", 4096, NULL, 3, NULL);
    xTaskCreate(ai_home_task, "ouo_ai_home", 8192, NULL, 3, &s_ai_home_task_handle);

    while (true) {
        ESP_LOGI(TAG, "alive uptime_ms=%" PRIu32 " mood=%s heap=%" PRIu32,
                 (uint32_t)(esp_timer_get_time() / 1000),
                 s_state.mood,
                 esp_get_free_heap_size());
        vTaskDelay(pdMS_TO_TICKS(5000));
    }
}
