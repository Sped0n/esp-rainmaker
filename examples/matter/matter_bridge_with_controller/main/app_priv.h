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

void on_device_list_update(void);


esp_err_t app_attribute_update_cb(esp_matter::attribute::callback_type_t type, uint16_t endpoint_id, uint32_t cluster_id,
                                          uint32_t attribute_id, esp_matter_attr_val_t *val, void *priv_data);

void app_matter_controller_set_rmaker_init_done(bool init_done);

esp_err_t app_matter_controller_attribute_update(uint32_t cluster_id, uint32_t attribute_id, esp_matter_attr_val_t *val);

esp_err_t app_identification_cb(esp_matter::identification::callback_type_t type, uint16_t endpoint_id, uint8_t effect_id,
                                         uint8_t effect_variant, void *priv_data);
