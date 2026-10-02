#pragma once

#include <freertos/FreeRTOS.h>
#include <freertos/event_groups.h>

#include <led_ctrl.hpp>
#include <esp_err.h>

#ifdef CONFIG_SI_SG_PROG_RIG
#include "current_tester.hpp"
#endif
#include "config/target_config.hpp"
#include "display_manager.hpp"
#include "procedure_executor.hpp"
#include "target_backend.hpp"

namespace flasher
{
    enum pg_state {
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
        CANCELLED = 9,
#ifdef CONFIG_SI_SG_PROG_RIG
        SG_CURRENT_TEST = 0xf0,
#endif
    };
} // namespace flasher

/**
 * Offline programming state machine. Family-agnostic: every target operation
 * is delegated to a target_backend selected from the job's target family.
 */
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
    led_ctrl &led = led_ctrl::instance();
    uint32_t written_len = 0;
    target_backend *backend = nullptr;

    display_manager *display = nullptr;
    ui_composer *composer = nullptr;

    volatile flasher::pg_state state = flasher::DETECT;
    flasher::pg_state failed_state = flasher::DONE;

#ifdef CONFIG_SI_SG_PROG_RIG
    current_tester pwr_test = {};
#endif

    static const constexpr char *TAG = "local_flasher";

public:
    /** Start a run of the active job. */
    void init();

    /**
     * Advance the run by one state. ESP_ERR_NOT_FINISHED while running,
     * ESP_OK when done, ESP_FAIL on error or after a cancel request.
     */
    esp_err_t handle_states();

    /** State that failed or was cancelled; DONE when the run passed. */
    flasher::pg_state get_failed_state() const
    {
        return failed_state;
    }

private:
    void select_backend();
    esp_err_t run_state();
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
    void on_cancelled();
    void check_cancel();

#ifdef CONFIG_SI_SG_PROG_RIG
    void on_current_test();
#endif
};
