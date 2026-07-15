#pragma once

#include <freertos/FreeRTOS.h>
#include <freertos/event_groups.h>
#include <esp_err.h>
#include <esp_log.h>
#include <ui/ui_if.hpp>
#include "ui_screen_progress_279.hpp"
#include "ui_screen_message_279.hpp"
#include "ui_screen_current_279.hpp"

class ui_composer_279 : public ui_composer
{
public:
    ui_composer_279() = default;

    esp_err_t init() override;
    esp_err_t display_init() override;
    esp_err_t display_erase(uint8_t percentage) override;
    esp_err_t display_test(size_t done, size_t total, const char *test_msg) override;
    esp_err_t display_program(uint8_t percentage) override;
    esp_err_t display_done() override;
    esp_err_t display_error(const char *header, const char *err_msg) override;
    esp_err_t display_config() override;
    esp_err_t display_current(double min_ua, double max_ua, double avg_ua, const char *state, lv_color_t state_color) override;

    esp_err_t wait_for_ui_mod(uint32_t wait_ticks = pdMS_TO_TICKS(1000)) const;

    void reload_base_obj();

public:
    ui_screen_current_279 current_screen = {}; // No time to make it polymorphic, just keep it simple for now...
    ui_screen_message_279 msg_screen = {};
    ui_screen_progress_279 progress_screen = {};

    lv_obj_t *base_obj = nullptr;
    ui_screen::state screen_state = ui_screen::CLEAR;

public:
    static const constexpr char TAG[] = "ui_composer";

};
