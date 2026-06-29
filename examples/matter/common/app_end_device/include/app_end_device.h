#pragma once

#include <button_gpio.h>
#include <esp_err.h>
#include <esp_matter.h>

/**
 * @brief Return the board button configuration used by the end-device examples.
 */
button_gpio_config_t app_end_device_button_driver_get_config(void);

/**
 * @brief Initialize the common Matter end-device platform pieces.
 *
 * Creates the Matter node/endpoints used by the selected example and installs the attribute and identification
 * callbacks provided by the application.
 */
esp_err_t app_end_device_init(esp_matter::attribute::callback_t app_attribute_update_cb,
                              esp_matter::identification::callback_t app_identification_cb);

/**
 * @brief Start the Matter stack for an initialized end-device example.
 */
esp_err_t app_end_device_start(esp_matter::event_callback_t app_event_cb);

/**
 * @brief Initialize RainMaker services for the end-device example.
 */
esp_err_t app_end_device_rmaker_init(void);

/**
 * @brief Start RainMaker after network provisioning setup is complete.
 */
esp_err_t app_end_device_rmaker_start(void);

/**
 * @brief Register Matter console commands used for local debugging.
 */
void app_end_device_enable_matter_console(void);
