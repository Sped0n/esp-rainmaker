/*
   This example code is in the Public Domain (or CC0 licensed, at your option.)

   Unless required by applicable law or agreed to in writing, this
   software is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR
   CONDITIONS OF ANY KIND, either express or implied.
*/

#include <app_controller_console.h>

#include <app_rmaker_matter_controller.h>
#include <esp_console.h>
#include <esp_err.h>
#include <esp_log.h>

extern "C" esp_err_t esp_rmaker_factory_reset(int seconds, int reboot_seconds);

static const char *TAG = "app_controller_console";

static int app_controller_list_devices_handler(int argc, char **argv)
{
    matter_device_t *device_list = app_rmaker_get_matter_device_list();
    if (!device_list) {
        ESP_LOGE(TAG, "Failed to get matter device list");
        return ESP_FAIL;
    }

    app_rmaker_print_matter_device_list(device_list);
    app_rmaker_free_matter_device_list(device_list);
    return ESP_OK;
}

static int app_controller_factory_reset_handler(int argc, char **argv)
{
    return esp_rmaker_factory_reset(2, 2);
}

extern "C" void app_controller_register_commands(void)
{
    const esp_console_cmd_t list_devices_cmd = {
        .command = "matter_device_list",
        .help = "Print the RainMaker Matter device list",
        .hint = NULL,
        .func = &app_controller_list_devices_handler,
    };
    ESP_ERROR_CHECK(esp_console_cmd_register(&list_devices_cmd));

    const esp_console_cmd_t factory_reset_cmd = {
        .command = "matter_factory_reset",
        .help = "Factory reset the device",
        .hint = NULL,
        .func = &app_controller_factory_reset_handler,
    };
    ESP_ERROR_CHECK(esp_console_cmd_register(&factory_reset_cmd));
}
