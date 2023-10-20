#pragma once

#include <freertos/FreeRTOS.h>
#include <freertos/event_groups.h>

#include <local_mission_manager.hpp>
#include <led_ctrl.hpp>
#include <esp_err.h>
#include "swd_prog.hpp"
#include "cdc_acm.hpp"

namespace flasher
{
    enum pg_state
    {
        DETECT = 0,
        ERASE = 1,
        PROGRAM = 2,
        ERROR = 3,
        VERIFY = 4,
        SELF_TEST = 5,
        DONE = 6,
    };

    enum event_bit : uint32_t {
        CLEAR_BUTTON_PRESSED = (1U << 0),
    };
}

class swd_headless_flasher
{
public:
    static swd_headless_flasher& instance()
    {
        static swd_headless_flasher instance;
        return instance;
    }

    swd_headless_flasher(swd_headless_flasher const &) = delete;
    void operator=(swd_headless_flasher const &) = delete;

private:
    swd_headless_flasher() = default;
    uint32_t written_len = 0;
    local_mission_manager &cfg_manager = local_mission_manager::instance();
    led_ctrl &led = led_ctrl::instance();
    swd_prog &swd = swd_prog::instance();
    EventGroupHandle_t flasher_evt = {};
    volatile flasher::pg_state state = flasher::DETECT;

    static const constexpr char *TAG = "swd_hdls_flr";

public:
    esp_err_t init();

private:
    void on_detect();
    void on_error();
    void on_erase();
    void on_program();
    void on_verify();
    void on_self_test();
    void on_done();

    static void button_isr(void *_ctx);
    static void button_intr_handler(void *_ctx);
};

