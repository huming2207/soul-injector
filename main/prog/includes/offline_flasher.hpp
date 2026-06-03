#pragma once

#include <freertos/FreeRTOS.h>
#include <freertos/event_groups.h>
#include <led_ctrl.hpp>
#include <esp_err.h>

#ifdef CONFIG_SI_SG_PROG_RIG
#include "current_tester.hpp"
#endif
#include "swd_prog.hpp"
#include "display_manager.hpp"
#include "procedure_executor.hpp"

namespace flasher
{
    enum pg_state
    {
        ERROR = -1,
        LOAD_ASSET = 0,
        PRE_PROGRAM = 1,
        DETECT = 2,
        ERASE = 3,
        PROGRAM = 4,
        VERIFY = 5,
        SELF_TEST = 6,
        POST_PROGRAM = 7,
        DONE = 8,
#ifdef CONFIG_SI_SG_PROG_RIG
        SG_CURRENT_TEST = 0xf0,
#endif
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
    bool asset_loaded = false;
    uint32_t written_len = 0;
    swd_prog *swd = swd_prog::instance();

    display_manager *display = nullptr;
    ui_composer *composer = nullptr;

    procedure_executor pre_program_steps = {};
    procedure_executor post_program_steps = {};

    volatile flasher::pg_state state = flasher::DETECT;

#ifdef CONFIG_SI_SG_PROG_RIG
    current_tester pwr_test = {};
#endif

    static const constexpr char *TAG = "local_flasher";
    static const constexpr char PRE_PROG_STEP_FILE[] = "/data/pre_prog.yaml";
    static const constexpr char POST_PROG_STEP_FILE[] = "/data/post_prog.yaml";

public:
    void init(bool force_reload_asset = false);
    esp_err_t handle_states();

private:
    void on_pre_program();
    void on_load_asset();
    void on_detect();
    void on_error();
    void on_erase();
    void on_program();
    void on_verify();
    void on_self_test();
    void on_post_program();
    void on_done();

#ifdef CONFIG_SI_SG_PROG_RIG
    void on_current_test();
#endif
};

