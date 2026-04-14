/*
   This example code is in the Public Domain (or CC0 licensed, at your option.)

   Unless required by applicable law or agreed to in writing, this
   software is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR
   CONDITIONS OF ANY KIND, either express or implied.
*/

#include <esp_err.h>
#include <esp_log.h>
#include <nvs_flash.h>

#include <esp_rmaker_core.h>
#include <esp_rmaker_ota.h>
#include <esp_rmaker_standard_devices.h>
#include <esp_rmaker_standard_params.h>
#include <esp_rmaker_standard_types.h>
#include <esp_rmaker_scenes.h>
#include <esp_rmaker_schedule.h>

#include <app_insights.h>
#include <app_matter.h>
#include <app_matter_bridge.h>
#include <app_matter_controller.h>
#include <esp_matter_console_bridge.h>
#include <app_matter_controller_callback.h>
#include <esp_matter_controller_console.h>
#include <app_matter_device_manager.h>
#include <app_priv.h>

using namespace esp_matter;
using namespace esp_matter::attribute;
using namespace chip::app::Clusters;

static const char *TAG = "app_main";

static matter_device_t *s_device_list = NULL;

void on_device_list_update(void)
{
    if (s_device_list) {
        free_matter_device_list(s_device_list);
    }
    s_device_list = fetch_device_list();
}

esp_err_t app_identification_cb(identification::callback_type_t type, uint16_t endpoint_id, uint8_t effect_id,
                                uint8_t effect_variant, void *priv_data)
{
    ESP_LOGI(TAG, "Identification callback: type: %d, effect: %d", type, effect_id);
    return ESP_OK;
}

esp_err_t app_attribute_update_cb(attribute::callback_type_t type, uint16_t endpoint_id, uint32_t cluster_id,
                                  uint32_t attribute_id, esp_matter_attr_val_t *val, void *priv_data)
{
    esp_err_t err = ESP_OK;
    if (type == PRE_UPDATE) {
        err = app_matter_bridge_attribute_update(endpoint_id, cluster_id, attribute_id, val);
    }

    if (type == POST_UPDATE) {
        err = app_matter_controller_attribute_update(cluster_id, attribute_id, val);
    }

    return err;
}

void app_event_cb(const ChipDeviceEvent *event, intptr_t arg)
{
    switch (event->Type) {
    case chip::DeviceLayer::DeviceEventType::PublicEventTypes::kCommissioningComplete:
        ESP_LOGI(TAG, "Commissioning complete");
        matter_controller_sync_node_id();
        app_matter_bridge_on_commissioning_complete();
        break;
    case chip::DeviceLayer::DeviceEventType::kFailSafeTimerExpired:
        ESP_LOGI(TAG, "Commissioning failed, fail safe timer expired");
        break;
    case chip::DeviceLayer::DeviceEventType::kCommissioningSessionStarted:
        ESP_LOGI(TAG, "Commissioning session started");
        break;
    case chip::DeviceLayer::DeviceEventType::kCommissioningSessionStopped:
        ESP_LOGI(TAG, "Commissioning session stopped");
        break;
    case chip::DeviceLayer::DeviceEventType::kCommissioningWindowOpened:
        ESP_LOGI(TAG, "Commissioning window opened");
        break;
    case chip::DeviceLayer::DeviceEventType::kCommissioningWindowClosed:
        ESP_LOGI(TAG, "Commissioning window closed");
        break;
    case chip::DeviceLayer::DeviceEventType::kInterfaceIpAddressChanged:
        if (event->InterfaceIpAddressChanged.Type == chip::DeviceLayer::InterfaceIpChangeType::kIpV4_Assigned) {
            matter_controller_handle_update();
        }
        break;

    default:
        break;
    }
}

extern "C" void app_main()
{
    /* System/NVS. */
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);

    /* Network + Matter. */
    ESP_ERROR_CHECK(app_matter_init(app_attribute_update_cb, app_identification_cb));
    ESP_ERROR_CHECK(app_matter_rmaker_init());
    ESP_ERROR_CHECK(app_matter_bridge_backend_init());
    ESP_ERROR_CHECK(app_matter_bridge_create());
    ESP_ERROR_CHECK(app_matter_bridge_init_demo_hooks());
    ESP_ERROR_CHECK(app_matter_start(app_event_cb));

    /* RainMaker init. */
    esp_rmaker_config_t rainmaker_cfg = {
        .enable_time_sync = false,
    };
    esp_rmaker_node_t *node = esp_rmaker_node_init(&rainmaker_cfg, "ESP RainMaker Device", "Controller");
    if (!node) {
        ESP_LOGE(TAG, "Could not initialise node.");
        vTaskDelay(5000 / portTICK_PERIOD_MS);
        abort();
    }

    esp_rmaker_device_t *device = esp_rmaker_device_create(CONTROLLER_DEVICE_NAME, ESP_RMAKER_DEVICE_ZIGBEE_GATEWAY, NULL);
    esp_rmaker_device_add_param(device,
                                esp_rmaker_name_param_create(ESP_RMAKER_DEF_NAME_PARAM, "MatterController"));
    esp_rmaker_node_add_device(node, device);

    /* Controller service / device manager. */
    ESP_ERROR_CHECK(matter_controller_enable(0x131B, app_matter_controller_callback));
    ESP_ERROR_CHECK(init_device_manager(on_device_list_update));

    /* Services. */
    esp_rmaker_ota_enable_default();
    esp_rmaker_timezone_service_enable();
    esp_rmaker_schedule_enable();
    esp_rmaker_scenes_enable();
    app_insights_enable();

    /* Start + console. */
    ESP_ERROR_CHECK(app_matter_rmaker_start());
    esp_rmaker_start();
    app_matter_controller_set_rmaker_init_done(true);

    app_matter_enable_matter_console();
    ESP_ERROR_CHECK(esp_matter::console::bridge_register_commands());
    ESP_ERROR_CHECK(esp_matter::console::controller_register_commands());
    ESP_ERROR_CHECK(esp_matter::console::ctl_dev_mgr_register_commands());
}
