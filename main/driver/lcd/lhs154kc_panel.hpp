#pragma once

#include <driver/spi_master.h>
#include <lvgl.h>
#include "disp_panel_if.hpp"

namespace lhs154kc
{
    typedef struct {
        uint8_t reg;
        uint8_t data[6];
        uint8_t len;
    } seq_t;
}

class lhs154kc_panel : public disp_panel_if
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
    esp_err_t spi_send(const uint8_t *payload, size_t len, bool is_cmd);
    esp_err_t spi_send(const uint16_t *pixel, size_t len);
    esp_err_t spi_send(uint8_t payload, bool is_cmd);
    esp_err_t spi_send(const lhs154kc::seq_t *seq);
    esp_err_t set_pos(uint16_t x1, uint16_t x2, uint16_t y1, uint16_t y2);

private:
    spi_device_handle_t device_handle = nullptr;
    lv_disp_drv_t lv_drv = {};

private:
    static constexpr const char TAG[] = "lhs154kc";
    static constexpr const uint8_t REG_SLEEP_OUT = 0x11;
    static constexpr const uint8_t REG_POS_GAMMA = 0xE0;
    static constexpr const uint8_t REG_NEG_GAMMA = 0xE1;
    static constexpr const uint8_t REG_DISP_INVERSION = 0x21;
    static constexpr const uint8_t REG_DISP_ON = 0x29;
    static constexpr const uint8_t POS_GAMMA_VAL[] = {
            0xD0,
            0x08,
            0x0E,
            0x09,
            0x09,
            0x05,
            0x31,
            0x33,
            0x48,
            0x17,
            0x14,
            0x15,
            0x31,
            0x34,
    };

    static constexpr const uint8_t NEG_GAMMA_VAL[] = {
            0xD0,
            0x08,
            0x0E,
            0x09,
            0x09,
            0x15,
            0x31,
            0x33,
            0x48,
            0x17,
            0x14,
            0x15,
            0x31,
            0x34,
    };

    static const constexpr lhs154kc::seq_t INIT_SEQ[] = {
            {0x36, {0x00}, 1},
            {0x3A, {0x05}, 1},
            {0xB2, {0x0C, 0x0C, 0x00, 0x33, 0x33}, 5},
            {0xB7, {0x35}, 1},
            {0xBB, {0x32}, 1}, // VCOM for LHS154KC is 1.35v!!!
            {0xC2, {0x01}, 1},
            {0xC3, {0x15}, 1}, // GVDD for LHS154KC is 4.8v!!
            {0xC4, {0x20}, 1},
            {0xC6, {0x0F}, 1},
            {0xD0, {0xA4, 0xA1}, 2},
    };
};
