#pragma once

#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/task.h>
#include <esp_err.h>
#include <esp_timer.h>
#include "si_ui.hpp"
#include "disp_panel_if.hpp"
#include "ui_composer_noop.hpp"

#ifdef CONFIG_SI_DISP_ENABLE
#include "lvgl.h"
#include "nt279vj_panel.hpp"
#include "ui_composer_279.hpp"
#endif

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
    si_ui *get_composer();

private:
#if defined(CONFIG_SI_DISP_ENABLE) && defined(CONFIG_SI_DISP_PANEL_NT279VJ)
    disp_panel_if *panel = (disp_panel_if *)(new nt279vj_panel());
    ui_composer_279 composer{};
#else
    // Other panels have no UI: the display stays off.
    disp_panel_if *panel = nullptr;
    ui_composer_noop composer{};
#endif

private:
    static const constexpr char TAG[] = "disp_mgr";
};
