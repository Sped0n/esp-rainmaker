/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include <cJSON.h>
#include <esp_err.h>
#include <stddef.h>

#include <lib/core/TLVReader.h>

extern "C" {

esp_err_t app_rmaker_matter_tlv_to_json(chip::TLV::TLVReader &reader, cJSON **json);
esp_err_t app_rmaker_matter_tlv_to_json_string(chip::TLV::TLVReader *reader, char *buf, size_t buf_size);

}
