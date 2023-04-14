#pragma once

#include <esp_err.h>
#include <lvgl.h>

namespace lcd
{
    enum state : uint32_t
    {
        STATE_SPLASH = 0,
        STATE_DETECT = 1,
        STATE_ERASE = 2,
        STATE_PROGRAM = 3,
        STATE_ERROR = 4,
        STATE_VERIFY = 5,
        STATE_DONE = 6,
    };
}

class lcd_manager
{
public:
    static lcd_manager *instance()
    {
        static lcd_manager _instance;
        return &_instance;
    }

    lcd_manager(lcd_manager const &) = delete;
    void operator=(lcd_manager const &) = delete;

public:
    esp_err_t init();
    esp_err_t display_splash();
    esp_err_t display_detect();
    esp_err_t display_erase();
    esp_err_t display_program(uint8_t percentage);
    esp_err_t display_error(const char *err_heading, const char *err_msg);
    esp_err_t display_verify(const char *verify_msg);
    esp_err_t display_done();

private:
    lcd_manager() = default;
    esp_err_t clear_display();
    esp_err_t draw_two_bars(lv_obj_t **top_out, lv_obj_t **bottom_out, lv_color_t top_color, lv_color_t bottom_color);

private:
    lcd::state curr_state = lcd::STATE_SPLASH;
    lv_obj_t *root_obj = nullptr;
    lv_obj_t *top_bar = nullptr;
    lv_obj_t *bottom_bar = nullptr;
};
