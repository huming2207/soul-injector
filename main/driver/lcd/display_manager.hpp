#pragma once

#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/task.h>
#include <esp_err.h>
#include "lvgl.h"
#include "nfp190b_panel.hpp"

class disp_panel_if
{
public:
    virtual esp_err_t init() = 0;
    virtual esp_err_t set_backlight(uint8_t level) = 0;
    virtual void flush_display(lv_disp_drv_t *disp_drv, const lv_area_t * area, lv_color_t * color_p) = 0;
    virtual esp_err_t deinit() = 0;
    [[nodiscard]] virtual size_t get_hor_size() const = 0;
    [[nodiscard]] virtual size_t get_ver_size() const = 0;
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
    static void handle_flush(lv_disp_drv_t * disp_drv, const lv_area_t * area, lv_color_t * color_p);
    static void IRAM_ATTR lv_tick_cb(void *arg);

private:
#ifdef CONFIG_SI_DISP_PANEL_NFP190B
    disp_panel_if *panel = (disp_panel_if *)(new nfp190b_panel());
#elif defined(CONFIG_SI_DISP_PANEL_LHS154KC)
    isp_panel_if *panel = (disp_panel_if *)(new nfp190b_panel());
#endif

private:
    static const constexpr char TAG[] = "disp_mgr";
    lv_disp_draw_buf_t draw_buf = {};
    uint8_t *disp_buf_a = nullptr;
    uint8_t *disp_buf_b = nullptr;
    SemaphoreHandle_t lv_ui_task_lock = nullptr;
    StaticTask_t lv_ui_task_stack = {};
    StackType_t *lv_ui_task_stack_buf = nullptr;
    TaskHandle_t lv_ui_task_handle = nullptr;
};
