#pragma once

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
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

private:
    led_strip_handle_t led = {};

public:
    esp_err_t init(gpio_num_t pin = (gpio_num_t)(CONFIG_SI_LED_SIGNAL_PIN))
    {
        led_strip_config_t led_config = {};
        led_config.strip_gpio_num = pin;
        led_config.max_leds = 1;
        led_config.led_pixel_format = LED_PIXEL_FORMAT_GRB;

#if defined(CONFIG_SI_LED_WS2812B)
        led_config.led_model = LED_MODEL_WS2812;
#elif defined(CONFIG_SI_LED_SK6812RGB)
        strip_config.led_model = LED_MODEL_SK6812;
#endif
        led_config.flags.invert_out = false;

        led_strip_rmt_config_t rmt_config = {};
        rmt_config.clk_src = RMT_CLK_SRC_DEFAULT;
        rmt_config.resolution_hz = 10 * 1000 * 1000;
        rmt_config.flags.with_dma = false; // We only have one LED so no DMA needed I guess?

        return led_strip_new_rmt_device((const led_strip_config_t *)&led_config, (const led_strip_rmt_config_t *)&rmt_config, &led);
    }

    esp_err_t set_color(uint8_t r, uint8_t g, uint8_t b, uint32_t wait_ms)
    {
        auto ret = led_strip_set_pixel(led, 0, r, g, b);
        ret = ret ?: led_strip_refresh(led);

        return ret;
    }
};

