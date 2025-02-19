#pragma once

#include <freertos/FreeRTOS.h>
#include <freertos/event_groups.h>
#include <led_ctrl.hpp>
#include <esp_err.h>
#include "swd_prog.hpp"
#include "display_manager.hpp"

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
}

class offline_flasher
{
public:
    static offline_flasher *instance()
    {
        static offline_flasher instance;
        return &instance;
    }

    offline_flasher(offline_flasher const &) = delete;
    void operator=(offline_flasher const &) = delete;

private:
    offline_flasher() = default;
    uint32_t written_len = 0;
    swd_prog *swd = swd_prog::instance();

    display_manager *display = nullptr;
    ui_composer *composer = nullptr;
    volatile flasher::pg_state state = flasher::DETECT;

    static const constexpr char *TAG = "local_flasher";

public:
    void init();
    esp_err_t handle_states();

private:
    void on_detect();
    void on_error();
    void on_erase();
    void on_program();
    void on_verify();
    void on_self_test();
    void on_done();
};

