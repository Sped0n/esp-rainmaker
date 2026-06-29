#include <esp_log.h>
#include <led_strip.h>

#include <led_driver.h>

static const char *TAG = "led_driver_rgb";
static bool current_power = false;
static uint8_t current_brightness = 0;
static uint16_t current_hue = 0;
static uint8_t current_saturation = 0;

static void hsv_to_rgb(uint16_t hue, uint8_t saturation, uint8_t brightness, uint8_t *red, uint8_t *green, uint8_t *blue)
{
    uint16_t h = hue % 360;
    uint16_t v = (brightness * 255) / 100;
    uint16_t c = (v * saturation) / 100;
    uint16_t h_mod = h % 120;
    uint16_t distance = h_mod > 60 ? h_mod - 60 : 60 - h_mod;
    uint16_t x = (c * (60 - distance)) / 60;
    uint16_t m = v - c;
    uint16_t r = 0;
    uint16_t g = 0;
    uint16_t b = 0;

    if (h < 60) {
        r = c;
        g = x;
    } else if (h < 120) {
        r = x;
        g = c;
    } else if (h < 180) {
        g = c;
        b = x;
    } else if (h < 240) {
        g = x;
        b = c;
    } else if (h < 300) {
        r = x;
        b = c;
    } else {
        r = c;
        b = x;
    }

    *red = r + m;
    *green = g + m;
    *blue = b + m;
}

static esp_err_t led_driver_refresh(led_driver_handle_t handle)
{
    led_strip_handle_t strip = (led_strip_handle_t)handle;
    if (!strip) {
        ESP_LOGE(TAG, "Invalid handle");
        return ESP_ERR_INVALID_ARG;
    }

    if (!current_power || current_brightness == 0) {
        return led_strip_clear(strip);
    }

    uint8_t red = 0;
    uint8_t green = 0;
    uint8_t blue = 0;
    hsv_to_rgb(current_hue, current_saturation, current_brightness, &red, &green, &blue);

    esp_err_t err = led_strip_set_pixel(strip, 0, red, green, blue);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "led_strip_set_pixel failed");
        return err;
    }
    return led_strip_refresh(strip);
}

led_driver_handle_t led_driver_init(led_driver_config_t *config)
{
    ESP_LOGI(TAG, "Initializing RGB light driver");

    led_strip_config_t strip_config = {
        .strip_gpio_num = config->gpio,
        .max_leds = 1,
        .led_model = LED_MODEL_WS2812,
        .color_component_format = LED_STRIP_COLOR_COMPONENT_FMT_GRB,
        .flags.invert_out = config->output_invert,
    };
    led_strip_rmt_config_t rmt_config = {
        .clk_src = RMT_CLK_SRC_DEFAULT,
        .resolution_hz = 10 * 1000 * 1000,
        .mem_block_symbols = 64,
        .flags.with_dma = false,
    };

    led_strip_handle_t strip = NULL;
    esp_err_t err = led_strip_new_rmt_device(&strip_config, &rmt_config, &strip);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "led_strip_new_rmt_device failed");
        return NULL;
    }

    led_strip_clear(strip);
    return (led_driver_handle_t)strip;
}

esp_err_t led_driver_set_power(led_driver_handle_t handle, bool power)
{
    current_power = power;
    return led_driver_refresh(handle);
}

esp_err_t led_driver_set_brightness(led_driver_handle_t handle, uint8_t brightness)
{
    if (brightness != 0) {
        current_brightness = brightness;
    }
    return led_driver_refresh(handle);
}

esp_err_t led_driver_set_hue(led_driver_handle_t handle, uint16_t hue)
{
    current_hue = hue;
    return led_driver_refresh(handle);
}

esp_err_t led_driver_set_saturation(led_driver_handle_t handle, uint8_t saturation)
{
    current_saturation = saturation;
    return led_driver_refresh(handle);
}

esp_err_t led_driver_set_temperature(led_driver_handle_t handle, uint32_t temperature)
{
    return ESP_ERR_NOT_SUPPORTED;
}

esp_err_t led_driver_set_xy(led_driver_handle_t handle, uint16_t x, uint16_t y)
{
    return ESP_ERR_NOT_SUPPORTED;
}
