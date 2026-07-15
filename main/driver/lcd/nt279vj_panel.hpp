#pragma once

#include <driver/spi_master.h>
#include <esp_lcd_panel_io.h>
#include <lvgl.h>
#include <esp_lvgl_port.h>

#include "disp_panel_if.hpp"

class nt279vj_panel : public disp_panel_if
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
    static const constexpr char TAG[] = "nt279vj_drv";
    static const constexpr spi_host_device_t LCD_SPI_HOST = SPI2_HOST;
    static const constexpr size_t LCD_H_RES = 428;
    static const constexpr size_t LCD_V_RES = 142;

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
