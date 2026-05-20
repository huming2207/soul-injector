#include <esp_log.h>
#include <esp_timer.h>
#include <algorithm>
#include "display_manager.hpp"
#include "ui_composer_114.hpp"

esp_err_t display_manager::init()
{
    ESP_LOGI(TAG, "Panel init");
    auto ret = panel->init();
    if (ret != ESP_OK) {
        return ret;
    }

    ESP_LOGI(TAG, "UI task init OK");

    ret = composer.init();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to set up UI composer 0x%x", ret);
        return ret;
    }

    ESP_LOGI(TAG, "Composer init OK");
    return ret;
}

disp_panel_if *display_manager::get_panel()
{
    return panel;
}

void display_manager::deinit()
{
    // TODO: not sure now
}

ui_composer *display_manager::get_composer()
{
    return &composer;
}
