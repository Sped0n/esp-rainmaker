/*
   This example code is in the Public Domain (or CC0 licensed, at your option.)

   Unless required by applicable law or agreed to in writing, this
   software is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR
   CONDITIONS OF ANY KIND, either express or implied.
*/

#pragma once

#include <esp_err.h>
#include <esp_matter.h>

/** Default attribute values used by Rainmaker during initialization */
#define CONTROLLER_DEVICE_NAME "Matter Controller"
#define DEFAULT_POWER true

typedef void *app_driver_handle_t;

/** Initialize the button driver
 *
 * This initializes the button driver associated with the selected board.
 *
 * @param[in] user_data Custom user data that will be used in button toggle callback.
 *
 * @return Handle on success.
 * @return NULL in case of failure.
 */
app_driver_handle_t app_driver_button_init(void *user_data);

void on_device_list_update(esp_err_t err);

esp_err_t update_device_refresh_ui_init();
