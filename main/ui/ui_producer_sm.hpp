#pragma once

#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include "ui_sm_if.hpp"

class ui_producer_sm : public ui_producer_sm_if
{
public:
    static ui_producer_sm *instance()
    {
        static ui_producer_sm _instance;
        return &_instance;
    }

    ui_producer_sm(ui_producer_sm const &) = delete;
    void operator=(ui_producer_sm const &) = delete;

public:
    esp_err_t init() override;
    esp_err_t deinit() override;

public:
    esp_err_t display(ui_state::init_screen *screen) override;
    esp_err_t display(ui_state::erase_screen *screen) override;
    esp_err_t display(ui_state::flash_screen *screen) override;
    esp_err_t display(ui_state::test_screen *screen) override;
    esp_err_t display(ui_state::error_screen *screen) override;

private:
    ui_producer_sm() = default;
    QueueHandle_t producer_queue = nullptr;

private:
    static const constexpr char TAG[] = "ui_prod";
};
