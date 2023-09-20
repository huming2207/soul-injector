#pragma once

#include <esp_err.h>

class disp_panel_if
{
public:
    virtual esp_err_t init() = 0;
    virtual esp_err_t set_backlight(uint8_t level) = 0;
    virtual void flush_display(lv_disp_drv_t *disp_drv, const lv_area_t * area, lv_color_t * color_p) = 0;
    virtual esp_err_t deinit() = 0;
    [[nodiscard]] virtual size_t get_hor_size() const = 0;
    [[nodiscard]] virtual size_t get_ver_size() const = 0;
    virtual lv_disp_drv_t *get_lv_disp_drv() = 0;
    virtual esp_err_t setup_lvgl(lv_disp_draw_buf_t *draw_buf) = 0;
};