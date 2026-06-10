/*
   This example code is in the Public Domain (or CC0 licensed, at your option.)

   Unless required by applicable law or agreed to in writing, this
   software is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR
   CONDITIONS OF ANY KIND, either express or implied.
*/

#include <app_matter_ctrl.h>
#include <app_matter_device_list.h>
#include <app_matter_onoff.h>
#include <ui_matter_ctrl.h>

#include <esp_check.h>
#include <esp_heap_caps.h>
#include <esp_log.h>
#include <esp_matter.h>
#include <esp_system.h>
#include <esp_wifi.h>
#include <freertos/FreeRTOS.h>
#include <freertos/idf_additions.h>
#include <freertos/task.h>
#include <nvs_flash.h>

#include <string.h>

static const char *TAG = "app_matter_ctrl";
static char s_qr_payload[160];
static bool s_is_provisioned;
static TaskHandle_t s_refresh_ui_task_handle;

static void matter_ctrl_rebuild_device_list_from_cache(void)
{
    if (matter_ctrl_is_provisioned()) {
        matter_onoff_ensure_retry_task_started();
    }

    matter_device_list_rebuild();
    ui_matter_config_update_cb(matter_ctrl_is_provisioned() ? UI_MATTER_EVT_REFRESH : UI_MATTER_EVT_LOADING);
    matter_onoff_subscribe_all();
}

void factory_reset(void)
{
    esp_wifi_restore();
    nvs_flash_deinit();
    nvs_flash_erase();
    esp_restart();
}

void matter_ctrl_change_state(intptr_t arg)
{
    matter_device_list_node_t *node = (matter_device_list_node_t *)arg;
    if (!node || !node->is_online) {
        return;
    }

    ESP_ERROR_CHECK_WITHOUT_ABORT(matter_onoff_toggle(node->node_id, node->endpoint_id));
}

const char *matter_ctrl_get_qr_payload(void)
{
    return s_qr_payload;
}

void matter_ctrl_set_qr_payload(const char *payload)
{
    if (!payload) {
        s_qr_payload[0] = 0;
        return;
    }
    strlcpy(s_qr_payload, payload, sizeof(s_qr_payload));
    ui_matter_config_update_cb(UI_MATTER_EVT_LOADING);
}

void matter_ctrl_set_provisioned(bool provisioned)
{
    s_is_provisioned = provisioned;
    if (provisioned) {
        matter_onoff_ensure_retry_task_started();
    }
    ui_matter_config_update_cb(provisioned ? UI_MATTER_EVT_REFRESH : UI_MATTER_EVT_LOADING);
}

bool matter_ctrl_is_provisioned(void)
{
    return s_is_provisioned;
}

void matter_ctrl_on_device_list_update(esp_err_t err)
{
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Device list update failed: %s", esp_err_to_name(err));
    }

    if (s_refresh_ui_task_handle) {
        xTaskNotifyGive(s_refresh_ui_task_handle);
    } else {
        matter_ctrl_rebuild_device_list_from_cache();
    }
}

static void refresh_ui_task(void *pvParameters)
{
    (void)pvParameters;

    while (true) {
        if (ulTaskNotifyTake(pdTRUE, portMAX_DELAY) == true) {
            matter_ctrl_rebuild_device_list_from_cache();
        }
    }
}

esp_err_t matter_ctrl_refresh_ui_init(void)
{
    BaseType_t ret = xTaskCreatePinnedToCoreWithCaps(refresh_ui_task, "refresh_ui", 4096, NULL, tskIDLE_PRIORITY,
                                                     &s_refresh_ui_task_handle, 1, MALLOC_CAP_SPIRAM);
    ESP_RETURN_ON_FALSE(ret == pdPASS, ESP_FAIL, TAG, "Failed to create refresh UI task");
    return ESP_OK;
}
