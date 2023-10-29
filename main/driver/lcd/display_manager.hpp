#pragma once

#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/task.h>
#include <esp_err.h>
#include <esp_timer.h>
#include "lvgl.h"
#include "nfp190b_panel.hpp"
#include "disp_panel_if.hpp"
#include "nfp114h_panel.hpp"
#include "ui_composer_114.hpp"

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
    void deinit();
    disp_panel_if *get_panel();
    QueueHandle_t get_ui_queue();

private:
    static void IRAM_ATTR lv_tick_cb(void *arg);
    static void lv_ui_task(void *_ctx);

private:
#ifdef CONFIG_SI_DISP_PANEL_NFP190B
    disp_panel_if *panel = (disp_panel_if *)(new nfp190b_panel());
#elif defined(CONFIG_SI_DISP_PANEL_LHS154KC)
    disp_panel_if *panel = (disp_panel_if *)(new nfp190b_panel());
#elif defined(CONFIG_SI_DISP_PANEL_NFP114H)
    disp_panel_if *panel = (disp_panel_if *)(new nfp114h_panel());
#endif

private:
    static const constexpr char TAG[] = "disp_mgr";
    static const constexpr size_t UI_STACK_SIZE = 131072;
    static const constexpr uint32_t LV_TICK_PERIOD_MS = 2;
    static const constexpr uint32_t LV_TASK_MAX_IDLE_MS = 500; // At least 2 refreshes guaranteed?? We don't need high FPS here...
    lv_disp_draw_buf_t draw_buf = {};
    uint8_t *disp_buf_a = nullptr;
    uint8_t *disp_buf_b = nullptr;
    StaticTask_t lv_ui_task_stack = {};
    StackType_t *lv_ui_task_stack_buf = nullptr;
    TaskHandle_t lv_ui_task_handle = nullptr;
    esp_timer_handle_t timer_handle = nullptr;
    QueueHandle_t ui_queue = nullptr;

    ui_composer_114 composer {};
};
