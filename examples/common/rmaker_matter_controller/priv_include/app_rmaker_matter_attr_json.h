/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include <cJSON.h>
#include <esp_rmaker_core.h>
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

void app_rmaker_matter_attr_json_set_param(esp_rmaker_param_t *matter_devices_param);
bool app_rmaker_matter_attr_json_update_tree(cJSON *root, uint16_t endpoint_id, uint32_t cluster_id,
                                             uint32_t attribute_id, const char *value);
void app_rmaker_matter_attr_json_publish_matter_devices_delta(cJSON *matter_devices_obj);
void app_rmaker_matter_attr_json_publish_online_delta(uint64_t node_id, const char *rainmaker_node_id, bool online);

#ifdef __cplusplus
}
#endif
