#pragma once

#include <driver/spi_master.h>
#include <esp_lcd_panel_io.h>
#include <lvgl.h>
#include <esp_lvgl_port.h>

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
    esp_err_t deinit() override;
    [[nodiscard]] size_t get_hor_size() const override;
    [[nodiscard]] size_t get_ver_size() const override;
    esp_err_t lock(uint32_t timeout_ms) override;
    void unlock() override;
    lv_display_t *get_lv_disp() override;

private:
    esp_err_t send_sequence(const nfp114h::seq_t *seq, size_t seq_cnt);

private:
    static const constexpr char TAG[] = "nfp114h_drv";
    static const constexpr spi_host_device_t LCD_SPI_HOST = SPI2_HOST;

#ifndef CONFIG_SI_DISP_PANEL_NFP114H_ALT_INIT_CFG
    static const constexpr nfp114h::seq_t LCD_INIT_SEQ[] = {
        // Black magic from ZJY
        {0x3a, {0x05}, 1},
        {0xb2, {0x0c, 0x0c, 0x00, 0x33, 0x33}, 5},
        {0xb7, {0x35}, 1},
        {0xbb, {0x19}, 1},
        {0xc0, {0x2c}, 1},
        {0xc2, {0x01}, 1},
        {0xc3, {0x12}, 1},
        {0xc4, {0x20}, 1},
        {0xc6, {0x0f}, 1},
        {0xd0, {0xa4, 0xa1}, 2},
        {0xe0, {0xD0, 0x04, 0x0D, 0x11, 0x13, 0x2B, 0x3F, 0x54, 0x4C, 0x18, 0x0D, 0x0B, 0x1F, 0x23}, 14},
        {0xe1, {0xD0, 0x04, 0x0C, 0x11, 0x13, 0x2C, 0x3F, 0x44, 0x51, 0x2F, 0x1F, 0x1F, 0x20, 0x23}, 14},
        {0x21, {}, 0},
        {0x29, {}, 0},
    };
#else
    static const constexpr nfp114h::seq_t LCD_INIT_SEQ[] = {
        // Black magic from another Taobao vendor
        {0x3a, {0x05}, 1},
        {0xb2, {0x0c, 0x0c, 0x00, 0x33, 0x33}, 5},
        {0xb7, {0x35}, 1},
        {0xBB, {0x3F}, 1}, // VCOM
        {0xC0, {0x2C}, 1}, // Power control
        {0xC2, {0x01}, 1}, // VDV and VRH Command Enable
        {0xC3, {0x0F}, 1}, // VRH Set
        {0xC4, {0x20}, 1}, // VDV Set
        {0xc6, {0x0f}, 1},
        {0xd0, {0xa4, 0xa1}, 2},
        {0xE0, {0xD0, 0x05, 0x09, 0x09, 0x08, 0x14, 0x28, 0x33, 0x3F, 0x07, 0x13, 0x14, 0x28, 0x30}, 15}, // Set Gamma
        {0xE1, {0xD0, 0x05, 0x09, 0x09, 0x08, 0x03, 0x24, 0x32, 0x32, 0x3B, 0x14, 0x13, 0x28, 0x2F}, 15}, // Set Gamma
        {0x21, {}, 0},
        {0x29, {}, 0},
    };
#endif

private:
    esp_lcd_panel_io_handle_t io_handle = nullptr;
    esp_lcd_panel_handle_t panel_handle = nullptr;
    lv_display_t *display = nullptr;

    static constexpr lvgl_port_cfg_t LVGL_CFG = {
        .task_priority = 3,       /* LVGL task priority */
        .task_stack = 32768,      /* LVGL task stack size */
        .task_affinity = -1,      /* LVGL task pinned to core (-1 is no affinity) */
        .task_max_sleep_ms = 500, /* Maximum sleep in LVGL task */
        .task_stack_caps = MALLOC_CAP_SPIRAM,
        .timer_period_ms = 5 /* LVGL timer tick period in ms */
    };
};
