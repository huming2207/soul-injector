#pragma once

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <esp_log.h>
#include <esp_err.h>
#include <driver/gpio.h>
#include <led_strip.h>

class led_ctrl
{
public:
    static led_ctrl& instance()
    {
        static led_ctrl instance;
        return instance;
    }

    led_ctrl(led_ctrl const &) = delete;
    void operator=(led_ctrl const &) = delete;

private:
    led_ctrl() = default;
    static constexpr char TAG[] = "led_ctrl";

private:
    led_strip_handle_t led = {};

public:
    esp_err_t init(gpio_num_t pin = (gpio_num_t)(CONFIG_SI_LED_SIGNAL_PIN))
    {
        ESP_LOGI(TAG, "init: LED pin=%ld", (int32_t)pin);
        led_strip_config_t led_config = {};
        led_config.strip_gpio_num = pin;
        led_config.max_leds = 1;
        led_config.color_component_format = LED_STRIP_COLOR_COMPONENT_FMT_RGB;

#if defined(CONFIG_SI_LED_WS2812B)
        led_config.led_model = LED_MODEL_WS2812;
        led_config.color_component_format = LED_STRIP_COLOR_COMPONENT_FMT_RGB;
#elif defined(CONFIG_SI_LED_SK6812RGB)
        led_config.led_model = LED_MODEL_SK6812;
        led_config.color_component_format = LED_STRIP_COLOR_COMPONENT_FMT_GRB;
#endif
        led_config.flags.invert_out = false;

        led_strip_rmt_config_t rmt_config = {};
        rmt_config.clk_src = RMT_CLK_SRC_DEFAULT;
        rmt_config.resolution_hz = 10 * 1000 * 1000;
        rmt_config.flags.with_dma = false; // We only have one LED so no DMA needed I guess?

        auto ret = led_strip_new_rmt_device(&led_config, &rmt_config, &led);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "init: failed to create LED strip on GPIO %ld: 0x%x %s", (int32_t)pin, ret, esp_err_to_name(ret));
            return ret;
        }

        return set_color(50, 0, 0);
    }

    esp_err_t set_color(uint8_t r, uint8_t g, uint8_t b)
    {
        if (led == nullptr) {
            ESP_LOGE(TAG, "set_color: LED strip is not initialized");
            return ESP_ERR_INVALID_STATE;
        }

        auto ret = led_strip_set_pixel(led, 0, r, g, b);
        ret = ret ?: led_strip_refresh(led);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "set_color: failed to set RGB(%u,%u,%u): 0x%x %s", r, g, b, ret, esp_err_to_name(ret));
            return ret;
        }

        return ESP_OK;
    }
};

