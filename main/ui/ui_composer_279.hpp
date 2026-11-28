#pragma once

#include <atomic>

#include <freertos/FreeRTOS.h>
#include <esp_err.h>
#include <lvgl.h>

#include "si_ui.hpp"
#include "ui_model.hpp"
#include "ui_view.hpp"

// The UI on the NT279VJ 2.79" panel (428 x 142).
//
// display_*() only stores the screen to show and returns: they never wait for
// the LVGL lock, so a run or a debug session is never held up by drawing. A
// timer in the LVGL task picks the screen up within POLL_MS and draws what
// changed. The same timer shows a debugger's session over the run's screen
// and the connected link in the header.
class ui_composer_279 : public si_ui
{
public:
    esp_err_t init() override;
    esp_err_t display_init() override;
    esp_err_t display_erase(uint8_t percentage) override;
    esp_err_t display_test(size_t done, size_t total, const char *test_msg) override;
    esp_err_t display_program(uint8_t percentage) override;
    esp_err_t display_done() override;
    esp_err_t display_error(const char *header, const char *err_msg) override;
    esp_err_t display_config() override;
    esp_err_t display_current(double min_ua, double max_ua, double avg_ua, const char *state, lv_color_t state_color) override;

    esp_err_t display_starting() override;
    esp_err_t display_connecting() override;
    esp_err_t display_verify() override;
    esp_err_t display_notice(const char *title, const char *detail) override;
    void set_progress(uint32_t done, uint32_t total) override;

    static constexpr uint32_t POLL_MS = 100;

private:
    esp_err_t post(const ui_model &model, bool ready = false);
    static void poll_cb(lv_timer_t *timer);
    void poll();

    // Written by any task, under lock
    portMUX_TYPE lock = portMUX_INITIALIZER_UNLOCKED;
    ui_model pending = {};
    bool pending_ready = false;
    std::atomic<uint32_t> pending_seq = 0;
    // Overrides the posted screen's progress: pending_seq << 8 | percent, so
    // progress counts only for the screen it was reported on. 0xff: none.
    std::atomic<uint32_t> step_progress = 0xff;

    // LVGL task only
    ui_view view;
    lv_timer_t *timer = nullptr;
    uint32_t taken_seq = 0;
    ui_model base = {};
    bool base_ready = false;

    static constexpr char TAG[] = "ui_composer";
};
