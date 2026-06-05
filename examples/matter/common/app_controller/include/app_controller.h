/*
   This example code is in the Public Domain (or CC0 licensed, at your option.)

   Unless required by applicable law or agreed to in writing, this
   software is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR
   CONDITIONS OF ANY KIND, either express or implied.
*/

#pragma once

#include <esp_err.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef void (*app_controller_device_list_update_cb_t)(esp_err_t err);

void app_controller_set_device_list_update_callback(app_controller_device_list_update_cb_t callback);

esp_err_t app_controller_init(void);
bool app_controller_is_ready(void);

#ifdef __cplusplus
}
#endif
