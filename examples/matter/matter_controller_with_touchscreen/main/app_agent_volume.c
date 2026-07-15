/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <audio_ng.h>
#include <esp_check.h>
#include <esp_log.h>
#include <esp_rmaker_standard_params.h>
#include <esp_rmaker_standard_types.h>

#include "app_agent_volume.h"

static const char *TAG = "app_agent_volume";
static esp_rmaker_param_t *s_volume_param;

#define AGENT_DEVICE_NAME "Espressif AI Agent"
#define AGENT_DEVICE_TYPE "AI Assistant"
#define VOLUME_PARAM_NAME "Volume"
#define VOLUME_PARAM_TYPE "esp.agent.param.volume"

static esp_err_t agent_device_write_cb(const esp_rmaker_device_t *device,
                                       const esp_rmaker_param_write_req_t write_req[],
                                       uint8_t count, void *priv_data,
                                       esp_rmaker_write_ctx_t *ctx)
{
    (void)device;
    (void)ctx;
    audio_ng_handle_t audio = priv_data;

    for (uint8_t i = 0; i < count; i++) {
        const esp_rmaker_param_t *param = write_req[i].param;
        esp_rmaker_param_val_t value = write_req[i].val;
        if (param != s_volume_param) {
            ESP_RETURN_ON_ERROR(esp_rmaker_param_update(param, value), TAG,
                                "Update agent parameter failed");
            continue;
        }
        ESP_RETURN_ON_FALSE(value.type == RMAKER_VAL_TYPE_INTEGER &&
                            value.val.i >= 0 && value.val.i <= 100,
                            ESP_ERR_INVALID_ARG, TAG, "Volume must be between 0 and 100");
        ESP_RETURN_ON_ERROR(audio_ng_set_volume(audio, (uint8_t)value.val.i), TAG,
                            "Set playback volume failed");
        ESP_RETURN_ON_ERROR(esp_rmaker_param_update(param, value), TAG,
                            "Persist playback volume failed");
        ESP_LOGI(TAG, "Playback volume set to %d", value.val.i);
    }
    return ESP_OK;
}

esp_err_t app_agent_volume_register(esp_rmaker_node_t *node, audio_ng_handle_t audio)
{
    ESP_RETURN_ON_FALSE(node != NULL, ESP_ERR_INVALID_ARG, TAG, "Invalid RainMaker node");

    uint8_t volume = 0;
    ESP_RETURN_ON_ERROR(audio_ng_get_volume(audio, &volume), TAG,
                        "Get initial playback volume failed");

    esp_rmaker_device_t *device = esp_rmaker_device_create(AGENT_DEVICE_NAME, AGENT_DEVICE_TYPE,
                                                            audio);
    ESP_RETURN_ON_FALSE(device != NULL, ESP_ERR_NO_MEM, TAG, "Create agent device failed");

    esp_err_t err = esp_rmaker_device_add_bulk_cb(device, agent_device_write_cb, NULL);
    if (err != ESP_OK) {
        goto fail;
    }
    esp_rmaker_param_t *name_param = esp_rmaker_name_param_create(ESP_RMAKER_DEF_NAME_PARAM,
                                                                  AGENT_DEVICE_NAME);
    if (name_param == NULL) {
        err = ESP_ERR_NO_MEM;
        goto fail;
    }
    err = esp_rmaker_device_add_param(device, name_param);
    if (err != ESP_OK) {
        goto fail;
    }

    s_volume_param = esp_rmaker_param_create(VOLUME_PARAM_NAME, VOLUME_PARAM_TYPE,
                                             esp_rmaker_int(volume),
                                             PROP_FLAG_READ | PROP_FLAG_WRITE | PROP_FLAG_PERSIST);
    if (s_volume_param == NULL) {
        err = ESP_ERR_NO_MEM;
        goto fail;
    }
    err = esp_rmaker_device_add_param(device, s_volume_param);
    if (err != ESP_OK) {
        goto fail;
    }
    err = esp_rmaker_param_add_ui_type(s_volume_param, ESP_RMAKER_UI_SLIDER);
    if (err != ESP_OK) {
        goto fail;
    }
    err = esp_rmaker_param_add_bounds(s_volume_param, esp_rmaker_int(0),
                                      esp_rmaker_int(100), esp_rmaker_int(1));
    if (err != ESP_OK) {
        goto fail;
    }
    err = esp_rmaker_node_add_device(node, device);
    if (err != ESP_OK) {
        goto fail;
    }
    return ESP_OK;

fail:
    s_volume_param = NULL;
    esp_rmaker_device_delete(device);
    ESP_LOGE(TAG, "Register agent device failed: %s", esp_err_to_name(err));
    return err;
}
