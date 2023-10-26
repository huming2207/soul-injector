#pragma once

#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include "ui_if.hpp"

class ui_commander
{
public:
    static ui_commander *instance()
    {
        static ui_commander _instance;
        return &_instance;
    }

    ui_commander(ui_commander const &) = delete;
    void operator=(ui_commander const &) = delete;

public:
    esp_err_t init();
    esp_err_t deinit();

public:
    esp_err_t display(ui_state::init_screen *screen);
    esp_err_t display(ui_state::erase_screen *screen);
    esp_err_t display(ui_state::flash_screen *screen);
    esp_err_t display(ui_state::test_screen *screen);
    esp_err_t display(ui_state::error_screen *screen);
    QueueHandle_t get_queue();

private:
    ui_commander() = default;
    QueueHandle_t producer_queue = nullptr;

private:
    static const constexpr char TAG[] = "ui_prod";
};
