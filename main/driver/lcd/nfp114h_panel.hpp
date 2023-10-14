#pragma once

#include <driver/spi_master.h>
#include <esp_lcd_panel_io.h>
#include <lvgl.h>

#include "disp_panel_if.hpp"

namespace nfp114h
{
    typedef struct {
        uint8_t reg;
        uint8_t data[16];
        uint8_t len;
    } seq_t;
}

class nfp114h_panel : public disp_panel_if
{
public:
    esp_err_t init() override;
    esp_err_t set_backlight(uint8_t level) override;
    void flush_display(lv_disp_drv_t *disp_drv, const lv_area_t * area, lv_color_t * color_p) override;
    esp_err_t deinit() override;
    [[nodiscard]] size_t get_hor_size() const override;
    [[nodiscard]] size_t get_ver_size() const override;
    lv_disp_drv_t *get_lv_disp_drv() override;
    lv_disp_t *get_lv_disp() override;
    esp_err_t setup_lvgl(lv_disp_draw_buf_t *draw_buf) override;

private:
    static bool handle_fb_trans_done(esp_lcd_panel_io_handle_t panel_io, esp_lcd_panel_io_event_data_t *edata, void *user_ctx);
    static void handle_flush(lv_disp_drv_t *disp_drv, const lv_area_t * area, lv_color_t * color_p);


    esp_err_t send_sequence(const nfp114h::seq_t *seq, size_t seq_cnt);

private:
    static const constexpr char TAG[] = "nfp114h_drv";
    static const constexpr spi_host_device_t LCD_SPI_HOST = SPI2_HOST;
    static const constexpr nfp114h::seq_t LCD_INIT_SEQ[] = { // Black magic from ZJY
            {0x3a, {0x05},1},
            {0xb2, {0x0c, 0x0c, 0x00, 0x33, 0x33}, 5},
            {0xb7, {0x35}, 1},
            {0xbb, {0x19}, 1},
            {0xc0, {0x2c}, 1},
            {0xc2, {0x01}, 1},
            {0xc3, {0x12}, 1},
            {0xc4, {0x20}, 1},
            {0xc6, {0x0f}, 1},
            {0xd0, {0xa4, 0xa1}, 2},
            {0xe0, {0xD0,0x04,0x0D,0x11,0x13,0x2B,0x3F,0x54,0x4C,0x18,0x0D,0x0B,0x1F,0x23}, 14},
            {0xe1, {0xD0,0x04,0x0C,0x11,0x13,0x2C,0x3F,0x44,0x51,0x2F,0x1F,0x1F,0x20,0x23}, 14},
            {0x21, {}, 0},
            {0x29, {}, 0},
    };

private:
    esp_lcd_i80_bus_handle_t bus_handle = nullptr;
    esp_lcd_panel_io_handle_t io_handle = nullptr;
    esp_lcd_panel_handle_t panel_handle = nullptr;
    lv_disp_drv_t lv_drv = {};
    lv_disp_t *lv_disp = nullptr;
};
