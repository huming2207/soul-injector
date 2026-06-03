#pragma once

#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/task.h>
#include <esp_err.h>
#include <esp_timer.h>
#include <ui/ui_if.hpp>
#include "disp_panel_if.hpp"
#include "ui_composer_noop.hpp"

#ifdef CONFIG_SI_DISP_ENABLE
#include "lvgl.h"
#include "nfp114h_panel.hpp"
#include "ui_composer_114.hpp"
#include "ui_screen_current_114.hpp"
#include "ui_screen_message_114.hpp"
#include "ui_screen_progress_114.hpp"
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
    ui_composer *get_composer();

private:
#ifdef CONFIG_SI_DISP_ENABLE
#ifdef CONFIG_SI_DISP_PANEL_NFP190B
    disp_panel_if *panel = nullptr;
#elif defined(CONFIG_SI_DISP_PANEL_LHS154KC)
    disp_panel_if *panel = nullptr;
#elif defined(CONFIG_SI_DISP_PANEL_NFP114H)
    disp_panel_if *panel = (disp_panel_if *)(new nfp114h_panel());
#endif
    ui_composer_114 composer {};
#else
    disp_panel_if *panel = nullptr;
    ui_composer_noop composer {};
#endif

private:
    static const constexpr char TAG[] = "disp_mgr";
};
