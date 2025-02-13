#pragma once

#include <freertos/FreeRTOS.h>
#include <freertos/event_groups.h>
#include <esp_err.h>
#include <esp_log.h>
#include <ui/ui_if.hpp>
#include "ui_screen_progress_114.hpp"
#include "ui_screen_message_114.hpp"
#include "ui_screen_current_114.hpp"

class ui_composer_114 : public ui_composer
{
public:
    ui_composer_114() = default;

    esp_err_t init();
    esp_err_t display_init();
    esp_err_t display_erase(uint8_t percentage);
    esp_err_t display_test(uint8_t percentage, const char *test_msg);
    esp_err_t display_program(uint8_t percentage);
    esp_err_t display_done();
    esp_err_t display_error(const char *header, const char *err_msg);
    esp_err_t display_config();
    esp_err_t display_current(float current_ua, float energy_mc, const char *state, lv_color_t state_color);

    esp_err_t wait_for_ui_mod(uint32_t wait_ticks = pdMS_TO_TICKS(1000)) const;

    void wait_and_render() override;
    void render_done() override;
    void reload_base_obj();
    void set_ready() const;
    void clear_ready() const;

public:
    EventGroupHandle_t evt_group = nullptr;
    ui_screen_current_114 current_screen = {}; // No time to make it polymorphic, just keep it simple for now...
    ui_screen_message_114 msg_screen = {};
    ui_screen_progress_114 progress_screen = {};

    lv_obj_t *base_obj = nullptr;
    ui_screen::state screen_state = ui_screen::CLEAR;

public:
    static const constexpr char TAG[] = "ui_composer";

};

