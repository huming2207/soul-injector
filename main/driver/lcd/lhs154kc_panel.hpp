#pragma once

#include <driver/spi_master.h>
#include <esp_lcd_panel_io.h>
#include <lvgl.h>
#include <esp_lvgl_port.h>

#include "disp_panel_if.hpp"

namespace lhs154kc
{
    typedef struct {
        uint8_t reg;
        uint8_t data[16];
        uint8_t len;
    } seq_t;
}

class lhs154kc_panel : public disp_panel_if
{
public:
    esp_err_t init() override;
    esp_err_t set_backlight(uint8_t level) override;
    esp_err_t deinit() override;
    [[nodiscard]] size_t get_hor_size() const override;
    [[nodiscard]] size_t get_ver_size() const override;
    esp_err_t lock(uint32_t timeout_ms) override;
    void unlock() override;
    lv_display_t *get_lv_disp() override;

private:
    esp_err_t send_sequence(const lhs154kc::seq_t *seq, size_t seq_cnt);

private:
    static const constexpr char TAG[] = "lhs154kc_drv";
    static const constexpr spi_host_device_t LCD_SPI_HOST = SPI2_HOST;

    static const constexpr lhs154kc::seq_t LCD_INIT_SEQ[] = {
            {0x36, {0x00}, 1},
            {0x3A, {0x05}, 1},
            {0xB2, {0x0C, 0x0C, 0x00, 0x33, 0x33}, 5},
            {0xB7, {0x35}, 1},
            {0xBB, {0x32}, 1}, // VCOM for LHS154KC is 1.35v
            {0xC2, {0x01}, 1},
            {0xC3, {0x15}, 1}, // GVDD for LHS154KC is 4.8v
            {0xC4, {0x20}, 1},
            {0xC6, {0x0F}, 1},
            {0xD0, {0xA4, 0xA1}, 2},
            {0xE0, {0xD0,0x08,0x0E,0x09,0x09,0x05,0x31,0x33,0x48,0x17,0x14,0x15,0x31,0x34}, 14},
            {0xE1, {0xD0,0x08,0x0E,0x09,0x09,0x15,0x31,0x33,0x48,0x17,0x14,0x15,0x31,0x34}, 14},
            {0x21, {}, 0},
            {0x29, {}, 0},
    };

private:
    esp_lcd_panel_io_handle_t io_handle = nullptr;
    esp_lcd_panel_handle_t panel_handle = nullptr;
    lv_display_t *display = nullptr;

    static constexpr lvgl_port_cfg_t LVGL_CFG = {
        .task_priority = 3,         /* LVGL task priority */
        .task_stack = 32768,        /* LVGL task stack size */
        .task_affinity = -1,        /* LVGL task pinned to core (-1 is no affinity) */
        .task_max_sleep_ms = 500,   /* Maximum sleep in LVGL task */
        .task_stack_caps = MALLOC_CAP_SPIRAM,
        .timer_period_ms = 5        /* LVGL timer tick period in ms */
    };
};
