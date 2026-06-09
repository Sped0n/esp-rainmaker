/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <esp_check.h>
#include <esp_err.h>
#include <esp_rmaker_core.h>

#include <app_rmaker_matter_rmctl.h>
#include <app_rmaker_matter_rmctl_internal.h>

#define TAG "rmaker_matter_rmctl"

static esp_rmaker_param_t *s_matter_devices_param = NULL;

esp_rmaker_param_t *app_rmaker_matter_controller_matter_devices_param_create(const char *param_name)
{
    s_matter_devices_param = esp_rmaker_param_create(param_name, "esp.param.matter-devices", esp_rmaker_obj("{}"),
                                                     PROP_FLAG_READ);
    return s_matter_devices_param;
}

esp_err_t app_rmaker_matter_rmctl_enable(void)
{
    ESP_RETURN_ON_FALSE(s_matter_devices_param, ESP_ERR_INVALID_STATE, TAG, "Matter-Devices param not created");
    ESP_RETURN_ON_ERROR(app_rmaker_matter_controller_cmd_resp_enable(), TAG, "Failed to enable command response");
    ESP_RETURN_ON_ERROR(app_rmaker_matter_controller_attr_report_enable(s_matter_devices_param), TAG,
                        "Failed to enable attribute report");
    return ESP_OK;
}
