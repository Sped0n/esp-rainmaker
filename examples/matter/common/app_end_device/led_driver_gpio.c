#include <driver/ledc.h>
#include <esp_log.h>
#include <hal/ledc_types.h>

#include <led_driver.h>

static const char *TAG = "led_driver_gpio";
static bool current_power = false;
static uint8_t current_brightness = 0;

led_driver_handle_t led_driver_init(led_driver_config_t *config)
{
    ESP_LOGI(TAG, "Initializing light driver");
    esp_err_t err = ESP_OK;

    ledc_timer_config_t ledc_timer = {
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .duty_resolution = LEDC_TIMER_8_BIT,
        .timer_num = LEDC_TIMER_1,
        .freq_hz = 5000,
        .clk_cfg = LEDC_AUTO_CLK,
    };
    err = ledc_timer_config(&ledc_timer);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "ledc_timer_config failed");
        return NULL;
    }

    ledc_channel_config_t ledc_channel = {
        .gpio_num = config->gpio,
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .channel = (ledc_channel_t)config->channel,
        .intr_type = LEDC_INTR_DISABLE,
        .timer_sel = LEDC_TIMER_1,
        .duty = 0,
        .hpoint = 0,
        .flags.output_invert = config->output_invert,
    };
    err = ledc_channel_config(&ledc_channel);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "ledc_channel_config failed");
        return NULL;
    }

    return (led_driver_handle_t)(config->channel + 1);
}

esp_err_t led_driver_set_power(led_driver_handle_t handle, bool power)
{
    current_power = power;
    return led_driver_set_brightness(handle, current_brightness);
}

esp_err_t led_driver_set_brightness(led_driver_handle_t handle, uint8_t brightness)
{
    int channel = (int)handle - 1;
    if (channel < 0) {
        ESP_LOGE(TAG, "Invalid handle");
        return ESP_ERR_INVALID_ARG;
    }

    if (brightness != 0) {
        current_brightness = brightness;
    }
    if (!current_power) {
        brightness = 0;
    }

    esp_err_t err = ledc_set_duty(LEDC_LOW_SPEED_MODE, (ledc_channel_t)channel, brightness);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "ledc_set_duty failed");
        return err;
    }
    err = ledc_update_duty(LEDC_LOW_SPEED_MODE, (ledc_channel_t)channel);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "ledc_update_duty failed");
    }
    return err;
}

esp_err_t led_driver_set_hue(led_driver_handle_t handle, uint16_t hue)
{
    return ESP_ERR_NOT_SUPPORTED;
}

esp_err_t led_driver_set_saturation(led_driver_handle_t handle, uint8_t saturation)
{
    return ESP_ERR_NOT_SUPPORTED;
}

esp_err_t led_driver_set_temperature(led_driver_handle_t handle, uint32_t temperature)
{
    return ESP_ERR_NOT_SUPPORTED;
}

esp_err_t led_driver_set_xy(led_driver_handle_t handle, uint16_t x, uint16_t y)
{
    return ESP_ERR_NOT_SUPPORTED;
}
