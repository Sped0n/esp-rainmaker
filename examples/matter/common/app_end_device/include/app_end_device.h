#pragma once

#include <button_gpio.h>
#include <esp_err.h>
#include <esp_matter.h>

button_gpio_config_t app_end_device_button_driver_get_config(void);

esp_err_t app_end_device_init(esp_matter::attribute::callback_t app_attribute_update_cb,
                              esp_matter::identification::callback_t app_identification_cb);
esp_err_t app_end_device_start(esp_matter::event_callback_t app_event_cb);
esp_err_t app_end_device_rmaker_init(void);
esp_err_t app_end_device_rmaker_start(void);
void app_end_device_enable_matter_console(void);
