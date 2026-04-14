/*
   This example code is in the Public Domain (or CC0 licensed, at your option.)

   Unless required by applicable law or agreed to in writing, this
   software is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR
   CONDITIONS OF ANY KIND, either express or implied.
*/

#include <esp_log.h>
#include <esp_matter_console.h>
#include <esp_matter_rainmaker.h>
#include <esp_rmaker_core.h>
#include <esp_rmaker_standard_params.h>

#include <app_matter.h>
#include <app_matter_controller.h>
#include <app_matter_noc_manager.h>
#include <app_priv.h>

using namespace esp_matter;
using namespace esp_matter::attribute;
using namespace chip::app::Clusters;

static const char *TAG = "app_matter";
static bool s_rmaker_init_done = false;

static const char *app_matter_get_rmaker_param_name_from_id(uint32_t cluster_id, uint32_t attribute_id)
{
    if (cluster_id == OnOff::Id) {
        if (attribute_id == OnOff::Attributes::OnOff::Id) {
            return ESP_RMAKER_DEF_POWER_NAME;
        }
    }
    return NULL;
}

static esp_rmaker_param_val_t app_matter_get_rmaker_val(esp_matter_attr_val_t *val, uint32_t cluster_id,
                                                        uint32_t attribute_id)
{
    if (cluster_id == OnOff::Id) {
        if (attribute_id == OnOff::Attributes::OnOff::Id) {
            return esp_rmaker_bool(val->val.b);
        }
    }
    return esp_rmaker_int(0);
}

void app_matter_controller_set_rmaker_init_done(bool init_done)
{
    s_rmaker_init_done = init_done;
}

esp_err_t app_matter_controller_attribute_update(uint32_t cluster_id, uint32_t attribute_id, esp_matter_attr_val_t *val)
{
    if (!s_rmaker_init_done) {
        ESP_LOGI(TAG, "RainMaker init not done. Not processing attribute update");
        return ESP_OK;
    }

    const char *device_name = CONTROLLER_DEVICE_NAME;
    const char *param_name = app_matter_get_rmaker_param_name_from_id(cluster_id, attribute_id);
    if (!param_name) {
        ESP_LOGD(TAG, "param name not handled");
        return ESP_FAIL;
    }

    const esp_rmaker_node_t *node = esp_rmaker_get_node();
    esp_rmaker_device_t *device = esp_rmaker_node_get_device_by_name(node, device_name);
    esp_rmaker_param_t *param = esp_rmaker_device_get_param_by_name(device, param_name);
    if (!param) {
        ESP_LOGE(TAG, "Param %s not found", param_name);
        return ESP_FAIL;
    }

    esp_rmaker_param_val_t rmaker_val = app_matter_get_rmaker_val(val, cluster_id, attribute_id);
    return esp_rmaker_param_update_and_report(param, rmaker_val);
}
