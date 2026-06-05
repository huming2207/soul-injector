#pragma once

#include <esp_lvgl_port.h>
#include <esp_err.h>

class disp_panel_if
{
public:
    virtual ~disp_panel_if() = default;

    virtual esp_err_t init() = 0;
    virtual esp_err_t set_backlight(uint8_t level) = 0;
    virtual esp_err_t deinit() = 0;
    [[nodiscard]] virtual size_t get_hor_size() const = 0;
    [[nodiscard]] virtual size_t get_ver_size() const = 0;
    virtual lv_display_t *get_lv_disp() = 0;
    virtual esp_err_t lock(uint32_t timeout_ms) = 0;
    virtual void unlock() = 0;
};
