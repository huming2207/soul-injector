#include "target_lines.hpp"

#include <driver/gpio.h>
#include <esp_cpu.h>
#include <esp_log.h>
#include <esp_pm.h>
#include <swd_host.h>
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

    if (swd_reset_held()) {
        return ESP_OK; // hold_reset_from_boot() set it up, connected: nRST goes through it
    }
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

void target_lines::drive_nrst(bool asserted)
{
#ifdef CONFIG_ESP_SWD_PHY_AXC2T245
    // A high GPIO turns on the reset MOSFET.
    gpio_set_level(static_cast<gpio_num_t>(CONFIG_ESP_SWD_NRST_PIN), asserted ? 1 : 0);
#else
    gpio_set_level(static_cast<gpio_num_t>(CONFIG_ESP_SWD_NRST_PIN), asserted ? 0 : 1);
#endif
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

// Light sleep isolates every GPIO: nRST and the switch would float and let the
// target go for each sleep. No light sleep while the hold lasts.
static esp_pm_lock_handle_t hold_pm_lock;
static bool hold_pm_held;

void target_lines::hold_reset_from_boot()
{
    if (hold_pm_lock != nullptr || esp_pm_lock_create(ESP_PM_NO_LIGHT_SLEEP, 0, "si_hold", &hold_pm_lock) == ESP_OK) {
        hold_pm_held = esp_pm_lock_acquire(hold_pm_lock) == ESP_OK;
    }
    swd_hold_reset(true); // nRST asserted before the switch connects it
    if (SWITCH_PIN != GPIO_NUM_NC) {
        gpio_config_t io_cfg = {};
        io_cfg.pin_bit_mask = BIT64(SWITCH_PIN);
        io_cfg.mode = GPIO_MODE_OUTPUT;
        gpio_set_level(SWITCH_PIN, 1);
        gpio_config(&io_cfg);
    }
}

void target_lines::end_boot_hold()
{
    // An attach under reset ends the hold itself (swd_connect_under_reset)
    // and owns the lines then; otherwise nRST is released here.
    if (swd_reset_held()) {
        swd_hold_reset(false);
        isolate();
        ESP_LOGI("target_lines", "hold from boot ended: target released");
    }
    if (hold_pm_held) {
        esp_pm_lock_release(hold_pm_lock);
        hold_pm_held = false;
    }
}
