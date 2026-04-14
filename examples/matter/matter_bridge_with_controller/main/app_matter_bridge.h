/*
   This example code is in the Public Domain (or CC0 licensed, at your option.)

   Unless required by applicable law or agreed to in writing, this
   software is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR
   CONDITIONS OF ANY KIND, either express or implied.
*/

#pragma once

#include <esp_err.h>
#include <esp_matter.h>

esp_err_t app_matter_bridge_create(void);
esp_err_t app_matter_bridge_backend_init(void);
esp_err_t app_matter_bridge_init_demo_hooks(void);
esp_err_t app_matter_bridge_attribute_update(uint16_t endpoint_id, uint32_t cluster_id, uint32_t attribute_id,
                                             esp_matter_attr_val_t *val);
void app_matter_bridge_on_commissioning_complete(void);
