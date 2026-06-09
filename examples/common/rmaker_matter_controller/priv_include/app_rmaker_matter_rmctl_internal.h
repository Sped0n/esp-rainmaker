/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include <esp_err.h>
#include <esp_rmaker_core.h>

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
#include <lib/core/TLVReader.h>

extern "C" {
#endif

esp_err_t app_rmaker_matter_controller_cmd_resp_enable(void);
esp_err_t app_rmaker_matter_controller_attr_report_enable(esp_rmaker_param_t *matter_devices_param);
void app_rmaker_matter_controller_attr_report_on_device_list_update(void);

#ifdef __cplusplus
}

void app_rmaker_matter_controller_decode_tlv_to_string(chip::TLV::TLVReader *data, char *buf, size_t buf_size);
#endif
