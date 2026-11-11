#include "target_lines.hpp"

#include <driver/gpio.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

static const constexpr gpio_num_t SWITCH_PIN = static_cast<gpio_num_t>(CONFIG_SI_TARGET_SWITCH_PIN);

esp_err_t target_lines::init()
{
    if (SWITCH_PIN == GPIO_NUM_NC) {
        return ESP_OK;
    }

    gpio_config_t io_cfg = {};
    io_cfg.pin_bit_mask = BIT64(SWITCH_PIN);
    io_cfg.mode = GPIO_MODE_OUTPUT;

    auto ret = gpio_set_level(SWITCH_PIN, 0);
    ret = ret ?: gpio_config(&io_cfg);
    if (ret == ESP_OK) {
        isolate();
    }
    return ret;
}

void target_lines::connect()
{
    if (SWITCH_PIN == GPIO_NUM_NC) {
        return;
    }

    set_swdio_pulldowns(false);
    gpio_set_level(SWITCH_PIN, 1);
    vTaskDelay(1); // Let the switch settle before the lines are used
}

void target_lines::isolate()
{
    if (SWITCH_PIN == GPIO_NUM_NC) {
        return;
    }

    gpio_set_level(SWITCH_PIN, 0);
    set_swdio_pulldowns(true);
}

void target_lines::set_swdio_pulldowns(bool enable)
{
#ifdef CONFIG_ESP_SWD_PHY_AXC2T245
    // The translator's host-side inputs must not float while nothing drives them.
    const auto out_pin = static_cast<gpio_num_t>(CONFIG_ESP_SWD_DATA_OUT_PIN);
    const auto in_pin = static_cast<gpio_num_t>(CONFIG_ESP_SWD_DATA_IN_PIN);
    if (enable) {
        gpio_pulldown_en(out_pin);
        gpio_pulldown_en(in_pin);
    } else {
        gpio_pulldown_dis(out_pin);
        gpio_pulldown_dis(in_pin);
    }
#endif
}
