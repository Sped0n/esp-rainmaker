/*
   This example code is in the Public Domain (or CC0 licensed, at your option.)

   Unless required by applicable law or agreed to in writing, this
   software is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR
   CONDITIONS OF ANY KIND, either express or implied.
*/

#include <esp_log.h>
#include <stdlib.h>
#include <string.h>

#include <app/server/Server.h>
#include <app_priv.h>
#include <button_gpio.h>
#include <driver/gpio.h>
#include <esp_check.h>
#include <esp_matter.h>
#include <esp_rmaker_core.h>
#include <esp_rmaker_standard_params.h>

#include "app_matter_ctrl.h"
#include "ui_matter_ctrl.h"

using namespace esp_matter;
using namespace chip::app::Clusters;

static const char *TAG = "app_driver";
TaskHandle_t xRefresh_Ui_Handle = NULL;
bool device_get_flag = false;

#define BUTTON_GPIO_PIN GPIO_NUM_0

typedef struct endpoint_type {
    uint16_t endpoint_id;
    struct endpoint_type *next;
} endpoint_type_t;

/* Be called after updating device list */
void on_device_list_update(esp_err_t err)
{
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Device list update failed: %s", esp_err_to_name(err));
    }
    device_get_flag = true;
    if (xRefresh_Ui_Handle) {
        xTaskNotifyGive(xRefresh_Ui_Handle);
    } else {
        matter_ctrl_refresh_device_list();
    }
}

app_driver_handle_t app_driver_button_init(void *user_data)
{
    (void)user_data;
    button_config_t btn_cfg = {};
    button_gpio_config_t gpio_cfg = {
        .gpio_num = BUTTON_GPIO_PIN,
        .active_level = 0,
        .enable_power_save = false,
    };
    button_handle_t handle = nullptr;
    esp_err_t err = iot_button_new_gpio_device(&btn_cfg, &gpio_cfg, &handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "iot_button_new_gpio_device failed: %s", esp_err_to_name(err));
        return nullptr;
    }
    return (app_driver_handle_t)handle;
}

static void refresh_ui_task(void *pvParameters)
{
    while (true) {
        if (ulTaskNotifyTake(pdTRUE, portMAX_DELAY) == true) {
            /* refresh ui */
            clean_screen_with_button();
            matter_ctrl_refresh_device_list();
        }
    }
}

esp_err_t update_device_refresh_ui_init()
{
    xTaskCreatePinnedToCore(refresh_ui_task, "refresh_ui", 4096, nullptr, tskIDLE_PRIORITY, &xRefresh_Ui_Handle, 1);
    if (xRefresh_Ui_Handle == NULL) {
        ESP_LOGE(TAG, "creat task for refresh ui failed!");
        return ESP_FAIL;
    }
    configASSERT(xRefresh_Ui_Handle);
    return ESP_OK;
}
