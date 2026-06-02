/*
   This example code is in the Public Domain (or CC0 licensed, at your option.)

   Unless required by applicable law or agreed to in writing, this
   software is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR
   CONDITIONS OF ANY KIND, either express or implied.
*/

#include <app_controller.h>
#include <app_controller_console.h>
#include <app_controller_op_creds_issuer.h>
#include <app_rmaker_matter_controller.h>
#include <esp_check.h>
#include <esp_err.h>
#include <esp_log.h>
#include <esp_matter_controller_console.h>
#include <nvs_flash.h>

static const char *TAG = "app_controller";
static app_controller_device_list_update_cb_t s_device_list_update_cb = NULL;

extern "C" void app_controller_set_device_list_update_callback(app_controller_device_list_update_cb_t callback)
{
    s_device_list_update_cb = callback;
}

extern "C" esp_err_t app_controller_init(void)
{
    static bool init_done = false;

    if (init_done) {
        return ESP_OK;
    }

    ESP_RETURN_ON_ERROR(nvs_flash_init(), TAG, "Failed to initialize NVS");
    ESP_RETURN_ON_ERROR(esp_matter::console::controller_register_commands(), TAG,
                        "Failed to register controller console commands");
    app_controller_register_commands();
    app_controller_register_op_creds_issuer();

    matter_controller_config_t controller_config = {
        .setup_callback = app_controller_client_setup,
        .update_noc_callback = app_controller_update_noc,
        .device_list_update_callback = s_device_list_update_cb,
    };

    ESP_RETURN_ON_ERROR(app_rmaker_matter_controller_enable(&controller_config), TAG,
                        "Failed to enable RainMaker Matter Controller");

    init_done = true;
    ESP_LOGI(TAG, "RainMaker Matter controller initialized");
    return ESP_OK;
}
