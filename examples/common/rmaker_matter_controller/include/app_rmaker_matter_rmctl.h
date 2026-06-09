/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include <esp_err.h>
#include <esp_rmaker_core.h>

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t app_rmaker_matter_rmctl_enable(void);

esp_rmaker_param_t *app_rmaker_matter_controller_matter_devices_param_create(const char *param_name);

#ifdef __cplusplus
}
#endif
