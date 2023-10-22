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

class ui_producer_sm_if
{
public:
    virtual esp_err_t init() = 0;
    virtual esp_err_t deinit() = 0;

public:
    // These functions below should be called in any thread except UI thread
    virtual esp_err_t display(ui_state::init_screen *screen)  = 0;
    virtual esp_err_t display(ui_state::erase_screen *screen) = 0;
    virtual esp_err_t display(ui_state::flash_screen *screen) = 0;
    virtual esp_err_t display(ui_state::test_screen *screen)  = 0;
    virtual esp_err_t display(ui_state::error_screen *screen) = 0;
};

class ui_consumer_sm_if
{
public:
    virtual esp_err_t init() = 0;
    virtual esp_err_t deinit() = 0;

public:
    // These functions below should be called in UI thread only
    virtual esp_err_t draw(ui_state::init_screen *screen)  = 0;
    virtual esp_err_t draw(ui_state::erase_screen *screen) = 0;
    virtual esp_err_t draw(ui_state::flash_screen *screen) = 0;
    virtual esp_err_t draw(ui_state::test_screen *screen)  = 0;
    virtual esp_err_t draw(ui_state::error_screen *screen) = 0;
};