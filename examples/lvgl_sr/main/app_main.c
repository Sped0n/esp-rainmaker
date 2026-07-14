#include <inttypes.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "dev_display_lcd.h"
#include "dev_lcd_touch.h"
#include "esp_board_manager.h"
#include "esp_board_manager_defs.h"
#include "esp_check.h"
#include "esp_log.h"
#include "esp_lv_adapter.h"
#include "esp_timer.h"
#include "lvgl.h"
#include "sr_owner.h"

static const char *TAG = "lvgl_sr";
static lv_display_t *s_display;
static lv_obj_t *s_fps_label;
static lv_obj_t *s_wake_label;
static lv_obj_t *s_buttons[4];

static esp_lv_adapter_rotation_t display_rotation(const dev_display_lcd_config_t *config)
{
    if (config->swap_xy) {
        return config->mirror_x ? ESP_LV_ADAPTER_ROTATE_90 : ESP_LV_ADAPTER_ROTATE_270;
    }
    return (config->mirror_x || config->mirror_y) ? ESP_LV_ADAPTER_ROTATE_180 : ESP_LV_ADAPTER_ROTATE_0;
}

#if CONFIG_ESP_BOARD_DEV_DISPLAY_LCD_SUB_RGB_SUPPORT || CONFIG_ESP_BOARD_DEV_DISPLAY_LCD_SUB_DSI_SUPPORT
static esp_lv_adapter_tear_avoid_mode_t tear_mode(uint8_t frame_buffers)
{
    if (frame_buffers >= 3) {
        return ESP_LV_ADAPTER_TEAR_AVOID_MODE_TRIPLE_PARTIAL;
    }
    if (frame_buffers == 2) {
        return ESP_LV_ADAPTER_TEAR_AVOID_MODE_DOUBLE_PARTIAL;
    }
    return ESP_LV_ADAPTER_TEAR_AVOID_MODE_NONE;
}
#endif

static lv_display_t *register_display(const dev_display_lcd_config_t *config,
                                      const dev_display_lcd_handles_t *handles)
{
    const esp_lv_adapter_rotation_t rotation = display_rotation(config);

    if (strcmp(config->sub_type, ESP_BOARD_DEVICE_LCD_SUB_TYPE_RGB) == 0) {
#if CONFIG_ESP_BOARD_DEV_DISPLAY_LCD_SUB_RGB_SUPPORT
        esp_lv_adapter_display_config_t display_config = ESP_LV_ADAPTER_DISPLAY_RGB_DEFAULT_CONFIG(
            handles->panel_handle, handles->io_handle, config->lcd_width, config->lcd_height, rotation);
        display_config.profile.use_psram = true;
        display_config.tear_avoid_mode = tear_mode(config->sub_cfg.rgb.panel_config.num_fbs);
        return esp_lv_adapter_register_display(&display_config);
#endif
    }
    if (strcmp(config->sub_type, ESP_BOARD_DEVICE_LCD_SUB_TYPE_DSI) == 0) {
#if CONFIG_ESP_BOARD_DEV_DISPLAY_LCD_SUB_DSI_SUPPORT
        esp_lv_adapter_display_config_t display_config = ESP_LV_ADAPTER_DISPLAY_MIPI_DEFAULT_CONFIG(
            handles->panel_handle, handles->io_handle, config->lcd_width, config->lcd_height, rotation);
        display_config.tear_avoid_mode = tear_mode(config->sub_cfg.dsi.dpi_config.num_fbs);
        return esp_lv_adapter_register_display(&display_config);
#endif
    }
    if (strcmp(config->sub_type, ESP_BOARD_DEVICE_LCD_SUB_TYPE_SPI) == 0 ||
        strcmp(config->sub_type, ESP_BOARD_DEVICE_LCD_SUB_TYPE_PARLIO) == 0) {
        esp_lv_adapter_display_config_t display_config = ESP_LV_ADAPTER_DISPLAY_SPI_WITH_PSRAM_DEFAULT_CONFIG(
            handles->panel_handle, handles->io_handle, config->lcd_width, config->lcd_height, rotation);
        return esp_lv_adapter_register_display(&display_config);
    }

    ESP_LOGE(TAG, "Unsupported display subtype: %s", config->sub_type);
    return NULL;
}

static esp_err_t platform_init(void)
{
    ESP_RETURN_ON_ERROR(esp_board_manager_init_device_by_name(ESP_BOARD_DEVICE_NAME_DISPLAY_LCD),
                        TAG, "Initialize display device failed");
#if CONFIG_ESP_BOARD_DEV_LCD_TOUCH_SUPPORT
    ESP_RETURN_ON_ERROR(esp_board_manager_init_device_by_name(ESP_BOARD_DEVICE_NAME_LCD_TOUCH),
                        TAG, "Initialize touch device failed");
#endif

    esp_lv_adapter_config_t adapter_config = ESP_LV_ADAPTER_DEFAULT_CONFIG();
    adapter_config.task_priority = 9;
    adapter_config.task_core_id = 1;
    ESP_RETURN_ON_ERROR(esp_lv_adapter_init(&adapter_config), TAG, "Initialize LVGL adapter failed");

    dev_display_lcd_handles_t *display_handles = NULL;
    dev_display_lcd_config_t *display_config = NULL;
    ESP_RETURN_ON_ERROR(esp_board_manager_get_device_handle(ESP_BOARD_DEVICE_NAME_DISPLAY_LCD,
                                                             (void **)&display_handles),
                        TAG, "Get display handle failed");
    ESP_RETURN_ON_ERROR(esp_board_manager_get_device_config(ESP_BOARD_DEVICE_NAME_DISPLAY_LCD,
                                                             (void **)&display_config),
                        TAG, "Get display config failed");
    s_display = register_display(display_config, display_handles);
    ESP_RETURN_ON_FALSE(s_display, ESP_FAIL, TAG, "Register display failed");

#if CONFIG_ESP_BOARD_DEV_LCD_TOUCH_SUPPORT
    dev_lcd_touch_handles_t *touch_handles = NULL;
    ESP_RETURN_ON_ERROR(esp_board_manager_get_device_handle(ESP_BOARD_DEVICE_NAME_LCD_TOUCH,
                                                             (void **)&touch_handles),
                        TAG, "Get touch handle failed");
    esp_lv_adapter_touch_config_t touch_config = ESP_LV_ADAPTER_TOUCH_DEFAULT_CONFIG(
        s_display, touch_handles->touch_handle);
    ESP_RETURN_ON_FALSE(esp_lv_adapter_register_touch(&touch_config), ESP_FAIL, TAG,
                        "Register touch input failed");
#endif

    ESP_RETURN_ON_ERROR(esp_lv_adapter_fps_stats_enable(s_display, true), TAG,
                        "Enable completed-frame FPS statistics failed");
    ESP_RETURN_ON_ERROR(esp_lv_adapter_start(), TAG, "Start LVGL adapter failed");
    ESP_LOGI(TAG, "Display ready: %dx%d", lv_display_get_horizontal_resolution(s_display),
             lv_display_get_vertical_resolution(s_display));
    return ESP_OK;
}

static void lifecycle_button_cb(lv_event_t *event)
{
    sr_intent_t intent = (sr_intent_t)(intptr_t)lv_event_get_user_data(event);
    esp_err_t err = sr_owner_submit(intent);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Submit lifecycle intent failed: %s", esp_err_to_name(err));
    }
}

static lv_obj_t *create_button(lv_obj_t *parent, const char *text, sr_intent_t intent)
{
    lv_obj_t *button = lv_button_create(parent);
    lv_obj_set_size(button, LV_PCT(100), 46);
    lv_obj_add_event_cb(button, lifecycle_button_cb, LV_EVENT_CLICKED, (void *)(intptr_t)intent);
    lv_obj_t *label = lv_label_create(button);
    lv_label_set_text(label, text);
    lv_obj_center(label);
    return button;
}

static void set_button_enabled(lv_obj_t *button, bool enabled)
{
    if (enabled) {
        lv_obj_remove_state(button, LV_STATE_DISABLED);
    } else {
        lv_obj_add_state(button, LV_STATE_DISABLED);
    }
}

static void snapshot_timer_cb(lv_timer_t *timer)
{
    sr_snapshot_t snapshot;
    sr_owner_get_snapshot(&snapshot);
    lv_label_set_text_fmt(s_wake_label, "Wake detections\n%" PRIu32, snapshot.wake_count);

    const bool stopped = snapshot.state == SR_STATE_STOPPED;
    const bool running = snapshot.state == SR_STATE_RUNNING;
    const bool paused = snapshot.state == SR_STATE_PAUSED;
    set_button_enabled(s_buttons[SR_INTENT_START], stopped);
    set_button_enabled(s_buttons[SR_INTENT_PAUSE], running);
    set_button_enabled(s_buttons[SR_INTENT_RESUME], paused);
    set_button_enabled(s_buttons[SR_INTENT_STOP], running || paused);
}

static void fps_timer_cb(lv_timer_t *timer)
{
    uint32_t fps = 0;
    esp_err_t err = esp_lv_adapter_get_fps(s_display, &fps);
    if (err == ESP_OK) {
        lv_label_set_text_fmt(s_fps_label, "Completed FPS\n%" PRIu32, fps);
    } else {
        ESP_LOGW(TAG, "Read FPS failed: %s", esp_err_to_name(err));
    }
}

static void hue_timer_cb(lv_timer_t *timer)
{
    lv_obj_t *hue_region = lv_timer_get_user_data(timer);
    uint32_t phase_ms = (uint32_t)((esp_timer_get_time() / 1000) % 2000);
    uint16_t hue = (uint16_t)(phase_ms * 360 / 2000);
    lv_obj_set_style_bg_color(hue_region, lv_color_hsv_to_rgb(hue, 85, 90), 0);
}

static void create_card(lv_obj_t *rail, unsigned index)
{
    static const uint32_t colors[] = {
        0xf94144, 0xf3722c, 0xf8961e, 0xf9c74f, 0x90be6d, 0x43aa8b,
        0x4d908e, 0x577590, 0x277da1, 0x6a4c93, 0xb56576, 0x355070,
    };

    lv_obj_t *card = lv_obj_create(rail);
    lv_obj_set_size(card, 132, LV_PCT(88));
    lv_obj_set_flex_flow(card, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(card, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_bg_color(card, lv_color_hex(colors[index]), 0);
    lv_obj_set_style_border_width(card, 0, 0);
    lv_obj_set_style_radius(card, 10, 0);
    lv_obj_remove_flag(card, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *name = lv_label_create(card);
    lv_label_set_text_fmt(name, "COLOR %02u", index + 1);
    lv_obj_set_style_text_color(name, lv_color_white(), 0);
    lv_obj_t *number = lv_label_create(card);
    lv_label_set_text_fmt(number, "%u", index + 1);
    lv_obj_set_style_text_color(number, lv_color_white(), 0);
}

static void create_ui(void)
{
    lv_obj_t *screen = lv_screen_active();
    lv_obj_set_style_bg_color(screen, lv_color_hex(0x10151d), 0);
    lv_obj_set_style_pad_all(screen, 8, 0);
    lv_obj_set_style_pad_gap(screen, 8, 0);
    lv_obj_set_flex_flow(screen, LV_FLEX_FLOW_ROW);

    lv_obj_t *left = lv_obj_create(screen);
    lv_obj_set_size(left, LV_PCT(66), LV_PCT(100));
    lv_obj_set_style_pad_all(left, 0, 0);
    lv_obj_set_style_pad_gap(left, 6, 0);
    lv_obj_set_style_border_width(left, 0, 0);
    lv_obj_set_style_bg_opa(left, LV_OPA_TRANSP, 0);
    lv_obj_set_flex_flow(left, LV_FLEX_FLOW_COLUMN);
    lv_obj_remove_flag(left, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *hue_region = lv_obj_create(left);
    lv_obj_set_width(hue_region, LV_PCT(100));
    lv_obj_set_flex_grow(hue_region, 7);
    lv_obj_set_style_border_width(hue_region, 0, 0);
    lv_obj_set_style_radius(hue_region, 12, 0);
    lv_obj_remove_flag(hue_region, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *rail = lv_obj_create(left);
    lv_obj_set_width(rail, LV_PCT(100));
    lv_obj_set_flex_grow(rail, 3);
    lv_obj_set_flex_flow(rail, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(rail, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_scroll_dir(rail, LV_DIR_HOR);
    lv_obj_set_scrollbar_mode(rail, LV_SCROLLBAR_MODE_AUTO);
    lv_obj_set_style_pad_all(rail, 6, 0);
    lv_obj_set_style_pad_gap(rail, 8, 0);
    lv_obj_set_style_bg_color(rail, lv_color_hex(0x202a38), 0);
    lv_obj_set_style_border_width(rail, 0, 0);
    lv_obj_set_style_radius(rail, 12, 0);
    for (unsigned i = 0; i < 12; ++i) {
        create_card(rail, i);
    }

    lv_obj_t *right = lv_obj_create(screen);
    lv_obj_set_height(right, LV_PCT(100));
    lv_obj_set_flex_grow(right, 1);
    lv_obj_set_flex_flow(right, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_all(right, 12, 0);
    lv_obj_set_style_pad_gap(right, 10, 0);
    lv_obj_set_style_bg_color(right, lv_color_hex(0x18212d), 0);
    lv_obj_set_style_border_width(right, 0, 0);
    lv_obj_set_style_radius(right, 12, 0);
    lv_obj_remove_flag(right, LV_OBJ_FLAG_SCROLLABLE);

    s_fps_label = lv_label_create(right);
    lv_label_set_text(s_fps_label, "Completed FPS\n0");
    lv_obj_set_style_text_color(s_fps_label, lv_color_hex(0x7dd3fc), 0);
    s_wake_label = lv_label_create(right);
    lv_label_set_text(s_wake_label, "Wake detections\n0");
    lv_obj_set_style_text_color(s_wake_label, lv_color_hex(0x86efac), 0);

    lv_obj_t *controls = lv_obj_create(right);
    lv_obj_set_width(controls, LV_PCT(100));
    lv_obj_set_flex_grow(controls, 1);
    lv_obj_set_flex_flow(controls, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(controls, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_all(controls, 0, 0);
    lv_obj_set_style_pad_gap(controls, 7, 0);
    lv_obj_set_style_bg_opa(controls, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(controls, 0, 0);
    lv_obj_remove_flag(controls, LV_OBJ_FLAG_SCROLLABLE);

    s_buttons[SR_INTENT_START] = create_button(controls, "Start", SR_INTENT_START);
    s_buttons[SR_INTENT_PAUSE] = create_button(controls, "Pause", SR_INTENT_PAUSE);
    s_buttons[SR_INTENT_RESUME] = create_button(controls, "Resume", SR_INTENT_RESUME);
    s_buttons[SR_INTENT_STOP] = create_button(controls, "Stop", SR_INTENT_STOP);

    lv_timer_create(hue_timer_cb, 33, hue_region);
    lv_timer_create(snapshot_timer_cb, 50, NULL);
    lv_timer_create(fps_timer_cb, 1000, NULL);
    snapshot_timer_cb(NULL);
}

void app_main(void)
{
    ESP_ERROR_CHECK(platform_init());
    ESP_ERROR_CHECK(sr_owner_init());
    ESP_ERROR_CHECK(esp_lv_adapter_lock(-1));
    create_ui();
    esp_lv_adapter_unlock();
    ESP_LOGI(TAG, "LVGL/SR concurrency demo ready; SR starts stopped");
}
