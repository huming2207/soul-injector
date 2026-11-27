#include <esp_log.h>
#include <esp_lvgl_port.h>

#include "agent_service.hpp"
#include "sidp_service.hpp"
#include "ui_composer_279.hpp"

esp_err_t ui_composer_279::init()
{
    if (timer != nullptr) {
        return ESP_OK; // display_manager and bootstrap_fsm both call init()
    }

    if (!lvgl_port_lock(1000)) {
        ESP_LOGE(TAG, "init: can't lock LVGL");
        return ESP_ERR_TIMEOUT;
    }

    view.create(lv_screen_active());
    base = ui_screens::booting();
    view.show(base);
    timer = lv_timer_create(poll_cb, POLL_MS, this);
    lvgl_port_unlock();

    if (timer == nullptr) {
        ESP_LOGE(TAG, "init: no memory for the timer");
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

esp_err_t ui_composer_279::post(const ui_model &model, bool ready)
{
    step_progress.store(-1, std::memory_order_relaxed); // belongs to the previous screen
    taskENTER_CRITICAL(&lock);
    pending = model;
    pending_ready = ready;
    pending_seq.fetch_add(1, std::memory_order_release);
    taskEXIT_CRITICAL(&lock);
    return ESP_OK;
}

void ui_composer_279::set_progress(uint32_t done, uint32_t total)
{
    const int8_t percent = total == 0 ? 100 : static_cast<int8_t>(static_cast<uint64_t>(done) * 100 / total);
    step_progress.store(percent > 100 ? 100 : percent, std::memory_order_relaxed);
}

void ui_composer_279::poll_cb(lv_timer_t *timer)
{
    static_cast<ui_composer_279 *>(lv_timer_get_user_data(timer))->poll();
}

void ui_composer_279::poll()
{
    const uint32_t seq = pending_seq.load(std::memory_order_acquire);
    if (seq != taken_seq) {
        taskENTER_CRITICAL(&lock);
        base = pending;
        base_ready = pending_ready;
        taken_seq = pending_seq.load(std::memory_order_relaxed);
        taskEXIT_CRITICAL(&lock);
    }

    ui_model shown = base;
    const int8_t progress = step_progress.load(std::memory_order_relaxed);
    if (shown.progress >= 0 && progress >= 0) {
        shown.progress = progress;
    }

    // A script watching the target from agent_service is worth saying only while idle.
    if (base_ready && agent_service::instance()->trap_has_target()) {
        shown = ui_screens::ready(true);
    }

    // A debugger owns the target exclusively: no run can be on screen meanwhile.
    switch (sidp_service::debug_state()) {
    case sidp::TARGET_HALTED:
        shown = ui_screens::debugging(ui_screens::debug_state::stopped);
        break;
    case sidp::TARGET_RUNNING:
        shown = ui_screens::debugging(ui_screens::debug_state::running);
        break;
    case sidp::TARGET_LOST:
        shown = ui_screens::debugging(ui_screens::debug_state::lost);
        break;
    case sidp::TARGET_DETACHED:
        break;
    }

    view.show(shown);

    ui_screens::link link = ui_screens::link::none;
    switch (sidp_service::connected_link()) {
    case sidp_service::link::usb:
        link = ui_screens::link::usb;
        break;
    case sidp_service::link::network:
        link = ui_screens::link::network;
        break;
    case sidp_service::link::none:
        break;
    }
    view.set_link(ui_screens::link_text(link));
}

esp_err_t ui_composer_279::display_init()
{
    return post(ui_screens::ready(false), true);
}

esp_err_t ui_composer_279::display_starting()
{
    return post(ui_screens::starting());
}

esp_err_t ui_composer_279::display_connecting()
{
    return post(ui_screens::connecting());
}

esp_err_t ui_composer_279::display_erase(uint8_t percentage)
{
    return post(ui_screens::erasing(percentage == UINT8_MAX ? -1 : percentage));
}

esp_err_t ui_composer_279::display_program(uint8_t percentage)
{
    return post(ui_screens::writing(percentage == UINT8_MAX ? -1 : percentage));
}

esp_err_t ui_composer_279::display_verify()
{
    return post(ui_screens::checking());
}

esp_err_t ui_composer_279::display_test(size_t done, size_t total, const char *test_msg)
{
    return post(ui_screens::testing(done, total, test_msg));
}

esp_err_t ui_composer_279::display_done()
{
    return post(ui_screens::passed());
}

esp_err_t ui_composer_279::display_error(const char *header, const char *err_msg)
{
    return post(ui_screens::failed(header, err_msg));
}

esp_err_t ui_composer_279::display_notice(const char *title, const char *detail)
{
    return post(ui_screens::notice(title, detail));
}

esp_err_t ui_composer_279::display_config()
{
    return post(ui_screens::setup());
}

esp_err_t ui_composer_279::display_current(double min_ua, double max_ua, double avg_ua, const char *state, lv_color_t state_color)
{
    (void)state_color; // the screen's colours are fixed
    return post(ui_screens::current(min_ua, max_ua, avg_ua, state));
}
