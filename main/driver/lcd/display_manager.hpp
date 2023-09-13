#pragma once

#include <esp_err.h>
#include "lvgl.h"

class disp_panel_if
{
public:
    virtual esp_err_t init() = 0;
    virtual esp_err_t set_backlight(uint8_t level) = 0;
    virtual void flush_display(lv_disp_drv_t *disp_drv, const lv_area_t * area, lv_color_t * color_p) = 0;
    virtual esp_err_t deinit() = 0;
};

class display_manager
{
public:
    static display_manager *instance()
    {
        static display_manager _instance;
        return &_instance;
    }

    void operator=(display_manager const &) = delete;

public:
    esp_err_t init();
    disp_panel_if *get_panel();

private:
    disp_panel_if *panel;
};
