#pragma once

#include "ui_sm_if.hpp"

class ui_consumer_sm_114 : public ui_consumer_sm_if
{
public:
    esp_err_t init() override;
    esp_err_t deinit() override;

public:
    // These functions below should be called in UI thread only
    esp_err_t draw(ui_state::init_screen *screen)  override;
    esp_err_t draw(ui_state::erase_screen *screen) override;
    esp_err_t draw(ui_state::flash_screen *screen) override;
    esp_err_t draw(ui_state::test_screen *screen)  override;
    esp_err_t draw(ui_state::error_screen *screen) override;
};