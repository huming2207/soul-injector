#pragma once

#include "si_ui.hpp"

class ui_composer_noop : public si_ui
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
    esp_err_t display_starting() override { return ESP_OK; }
    esp_err_t display_connecting() override { return ESP_OK; }
    esp_err_t display_verify() override { return ESP_OK; }
    esp_err_t display_finishing() override { return ESP_OK; }
    esp_err_t display_notice(const char *, const char *) override { return ESP_OK; }
    void set_progress(uint32_t, uint32_t) override {}
};
