#pragma once

#include <esp_err.h>
#include <esp_log.h>

namespace ui_state
{
    struct init_screen
    {

    };

    struct erase_screen
    {

    };

    struct flash_screen
    {

    };

    struct test_screen
    {

    };

    struct error_screen
    {

    };

};

class ui_state_machine_if
{
public:
    virtual esp_err_t init() = 0;
    virtual esp_err_t deinit() = 0;

public:
    virtual esp_err_t display() = 0;
};