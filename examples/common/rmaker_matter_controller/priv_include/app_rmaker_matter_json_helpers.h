/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include <cJSON.h>
#include <esp_err.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <lib/core/TLVReader.h>

extern "C" {

esp_err_t app_rmaker_matter_tlv_to_json(chip::TLV::TLVReader &reader, cJSON **json);
esp_err_t app_rmaker_matter_tlv_to_json_string(chip::TLV::TLVReader *reader, char *buf, size_t buf_size);

bool app_rmaker_matter_json_pending_is_empty(cJSON *pending_root);
bool app_rmaker_matter_json_merge_pending_attr(cJSON **pending_root, uint64_t node_id,
                                               const char *rainmaker_node_id, uint16_t endpoint_id,
                                               uint32_t cluster_id, uint32_t attribute_id, const char *value_json);
cJSON *app_rmaker_matter_json_detach_pending_all(cJSON **pending_root);
cJSON *app_rmaker_matter_json_detach_pending_node(cJSON **pending_root, uint64_t node_id);

}
