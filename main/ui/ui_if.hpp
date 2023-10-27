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

    enum display_state : int8_t
    {
        STATE_EMPTY = -1,
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
        uint8_t total_count;
        uint32_t bg_color;
        char comment[32];
    };
};

class ui_composer_sm
{
public:
    virtual esp_err_t init() = 0;

public:
    // These functions below should be called in UI thread only
    virtual esp_err_t draw_init(ui_state::queue_item *screen)  = 0;
    virtual esp_err_t draw_erase(ui_state::queue_item *screen) = 0;
    virtual esp_err_t draw_flash(ui_state::queue_item *screen) = 0;
    virtual esp_err_t draw_test(ui_state::queue_item *screen)  = 0;
    virtual esp_err_t draw_error(ui_state::queue_item *screen) = 0;
    virtual esp_err_t draw_done(ui_state::queue_item *screen) = 0;
};