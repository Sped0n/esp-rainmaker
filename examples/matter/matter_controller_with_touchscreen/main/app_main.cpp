/*
   This example code is in the Public Domain (or CC0 licensed, at your option.)

   Unless required by applicable law or agreed to in writing, this
   software is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR
   CONDITIONS OF ANY KIND, either express or implied.
*/

#include <app_matter_ctrl.h>
#include <app_matter_device_list.h>
#include <app_matter_onoff.h>
#include <box_main.h>
#include <ui_main.h>
#include <ui_matter_ctrl.h>

#include <app_controller.h>
#include <app_insights.h>
#include <app_network.h>
#include <app_rmaker_matter_controller.h>
#include <esp_event.h>
#include <esp_log.h>
#include <esp_matter.h>
#include <esp_matter_console.h>
#include <esp_rmaker_auth_service.h>
#include <esp_rmaker_core.h>
#include <esp_rmaker_ota.h>
#include <esp_rmaker_scenes.h>
#include <esp_rmaker_schedule.h>
#include <esp_rmaker_standard_services.h>
#include <esp_wifi.h>
#include <nvs_flash.h>
#include <network_provisioning/manager.h>
#include <protocomm_security.h>

#include <string.h>

#if CONFIG_OPENTHREAD_BORDER_ROUTER
#include <esp_rmaker_thread_br.h>
#include <esp_ot_config.h>
#endif

static const char *TAG = "app_main";

static void wifi_status_event_handler(void *arg, esp_event_base_t event_base, int32_t event_id, void *event_data)
{
    (void)arg;
    (void)event_data;

    if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ui_acquire();
        ui_main_status_bar_set_wifi(true);
        ui_release();
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        ui_acquire();
        ui_main_status_bar_set_wifi(false);
        ui_release();
    }
}

static void app_network_event_handler(void *arg, esp_event_base_t event_base, int32_t event_id, void *event_data)
{
    if (event_base == NETWORK_PROV_EVENT) {
        switch (event_id) {
        case NETWORK_PROV_WIFI_CRED_RECV:
            ui_matter_config_update_cb(UI_MATTER_EVT_PROVISIONING);
            break;
        default:
            break;
        }
        return;
    }

    if (event_base == PROTOCOMM_SECURITY_SESSION_EVENT) {
        if (event_id == PROTOCOMM_SECURITY_SESSION_SETUP_OK) {
            ui_matter_config_update_cb(UI_MATTER_EVT_PROVISIONING);
        }
        return;
    }

    if (event_base != APP_NETWORK_EVENT) {
        return;
    }

    switch (event_id) {
    case APP_NETWORK_EVENT_QR_DISPLAY:
        matter_ctrl_set_qr_payload((const char *)event_data);
        matter_ctrl_set_provisioned(false);
        break;
    case APP_NETWORK_EVENT_PROV_TIMEOUT:
    case APP_NETWORK_EVENT_PROV_RESTART:
    case APP_NETWORK_EVENT_PROV_CRED_MISMATCH:
        matter_ctrl_set_provisioned(false);
        break;
    default:
        break;
    }
}

extern "C" void app_main()
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);

    app_network_init();
    ESP_ERROR_CHECK(esp_event_handler_register(APP_NETWORK_EVENT, ESP_EVENT_ANY_ID, &app_network_event_handler, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(NETWORK_PROV_EVENT, ESP_EVENT_ANY_ID, &app_network_event_handler, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(PROTOCOMM_SECURITY_SESSION_EVENT, ESP_EVENT_ANY_ID,
                                               &app_network_event_handler, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &wifi_status_event_handler, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, WIFI_EVENT_STA_DISCONNECTED, &wifi_status_event_handler,
                                               NULL));

    esp_rmaker_config_t rainmaker_cfg = {
        .enable_time_sync = false,
    };
    esp_rmaker_node_t *node = esp_rmaker_node_init(&rainmaker_cfg, "ESP RainMaker Device", "Controller");
    if (!node) {
        ESP_LOGE(TAG, "Could not initialise node. Aborting!!!");
        vTaskDelay(5000 / portTICK_PERIOD_MS);
        abort();
    }

    esp_rmaker_system_serv_config_t system_serv_config = {
        .flags = SYSTEM_SERV_FLAGS_ALL,
        .reboot_seconds = 0,
        .reset_seconds = 2,
        .reset_reboot_seconds = 0,
    };
    esp_rmaker_system_service_enable(&system_serv_config);

    esp_rmaker_device_t *device = esp_rmaker_device_create("MatterController", "matter-controller", NULL);
    ESP_ERROR_CHECK(app_controller_set_device_params(device));
    ESP_ERROR_CHECK(esp_rmaker_node_add_device(node, device));

    esp_rmaker_ota_enable_default();
    esp_rmaker_timezone_service_enable();
    esp_rmaker_schedule_enable();
    esp_rmaker_scenes_enable();
    app_insights_enable();
#if CONFIG_OPENTHREAD_BORDER_ROUTER
    esp_openthread_platform_config_t thread_cfg = {
        .radio_config = ESP_OPENTHREAD_DEFAULT_RADIO_CONFIG(),
        .host_config = ESP_OPENTHREAD_DEFAULT_HOST_CONFIG(),
        .port_config = ESP_OPENTHREAD_DEFAULT_PORT_CONFIG(),
    };
    ESP_ERROR_CHECK(esp_rmaker_thread_br_enable(&thread_cfg));
#endif

    app_controller_set_device_list_update_cb(matter_ctrl_on_device_list_update);
    ESP_ERROR_CHECK(app_controller_init());

    box_main();

    esp_rmaker_auth_service_enable();
    esp_rmaker_start();

    ESP_ERROR_CHECK(app_network_set_custom_mfg_data(MFG_DATA_DEVICE_TYPE_MATTER_CONTROLLER,
                                                    MFG_DATA_DEVICE_SUBTYPE_MATTER_CONTROLLER));
    err = app_network_start(POP_TYPE_RANDOM);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Could not start Wi-Fi. Aborting!!!");
        vTaskDelay(5000 / portTICK_PERIOD_MS);
        abort();
    }

    matter_ctrl_set_provisioned(true);
    ESP_ERROR_CHECK(esp_matter::start(NULL));
    esp_matter::console::diagnostics_register_commands();
    esp_matter::console::init();

    ESP_ERROR_CHECK(matter_ctrl_refresh_ui_init());
    app_rmaker_matter_controller_handle_update();
    matter_device_list_rebuild();
    ui_matter_config_update_cb(matter_ctrl_is_provisioned() ? UI_MATTER_EVT_REFRESH : UI_MATTER_EVT_LOADING);
    matter_onoff_subscribe_all();
}
