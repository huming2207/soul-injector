#pragma once

#include <esp_err.h>
#include <esp_log.h>

namespace ui_state
{
    struct init_screen
    {
        char title[32];
        char subtitle[32];
    };

    struct erase_screen
    {
        uint8_t percentage;
        char title[32];
        char subtitle[32];
    };

    struct flash_screen
    {
        uint8_t percentage;
        char title[32];
        char subtitle[32];
    };

    struct test_screen
    {
        char title[32];
        char subtitle[32];
    };

    struct error_screen
    {
        char title[32];
        char subtitle[32];
    };

    enum display_state : uint8_t
    {
        STATE_INIT = 0,
        STATE_ERASE = 1,
        STATE_FLASH = 2,
        STATE_TEST  = 3,
        STATE_ERROR = 4,
    };

    struct __attribute__((packed)) queue_item
    {
        display_state state;
        uint8_t percentage;
        uint8_t bg_opacity;
        uint32_t bg_color;
        char title[32];
        char subtitle[32];
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