/*
 * SPDX-FileCopyrightText: 2015-2022 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Unlicense OR CC0-1.0
 */

#pragma once

#include <stdbool.h>
#include "esp_err.h"
#include "box_main.h"
#include "box_platform.h"
#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    lv_style_t style;
    lv_style_t style_focus_no_outline;
    lv_style_t style_focus;
    lv_style_t style_pr;
} button_style_t;

typedef enum {
    UI_MAIN_VOICE_HIDDEN,
    UI_MAIN_VOICE_READY,
    UI_MAIN_VOICE_WAITING,
    UI_MAIN_VOICE_ACTIVE,
} ui_main_voice_state_t;

typedef struct {
    bool is_large;
    lv_coord_t display_width;
    lv_coord_t display_height;
    lv_coord_t status_bar_height;
    lv_coord_t page_width;
    lv_coord_t page_height;
    uint16_t scale;
    const lv_font_t *body_font;
    const lv_font_t *heading_font;
} ui_layout_t;

typedef void (*ui_main_voice_action_cb_t)(bool stop, void *user_data);

esp_err_t ui_main_start(void);
void ui_acquire(void);
void ui_release(void);
lv_group_t *ui_get_btn_op_group(void);
button_style_t *ui_button_styles(void);
const ui_layout_t *ui_main_get_layout(void);
lv_coord_t ui_layout_scale(lv_coord_t compact_size);
lv_obj_t *ui_main_get_status_bar(void);
void ui_main_status_bar_set_wifi(bool is_connected);
void ui_main_status_bar_set_cloud(bool is_connected);
void ui_main_status_bar_set_voice_state(ui_main_voice_state_t state);
void ui_main_status_bar_set_voice_action_callback(ui_main_voice_action_cb_t callback, void *user_data);

#ifdef __cplusplus
}
#endif
