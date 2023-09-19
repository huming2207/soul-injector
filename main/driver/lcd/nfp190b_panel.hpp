#pragma once

#include <esp_lcd_panel_io.h>
#include "display_manager.hpp"

class nfp190b_panel : public disp_panel_if
{
public:
    esp_err_t init() override;
    esp_err_t set_backlight(uint8_t level) override;
    void flush_display(lv_disp_drv_t *disp_drv, const lv_area_t * area, lv_color_t * color_p) override;
    esp_err_t deinit() override;
    [[nodiscard]] size_t get_hor_size() const override;
    [[nodiscard]] size_t get_ver_size() const override;
    lv_disp_drv_t *get_lv_disp_drv() override;
    esp_err_t setup_lvgl(lv_disp_draw_buf_t *draw_buf) override;

private:
    static bool handle_fb_trans_finish(esp_lcd_panel_io_handle_t panel_io, esp_lcd_panel_io_event_data_t *edata, void *user_ctx);
    static void handle_flush(lv_disp_drv_t *disp_drv, const lv_area_t * area, lv_color_t * color_p);

private:
    static const constexpr char TAG[] = "nfp190b_drv";

private:
    esp_lcd_i80_bus_handle_t bus_handle = nullptr;
    esp_lcd_panel_io_handle_t io_handle = nullptr;
    esp_lcd_panel_handle_t panel_handle = nullptr;
    lv_disp_drv_t lv_drv = {};
    lv_disp_t *lv_disp = nullptr;
};

