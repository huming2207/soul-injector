#include <esp_log.h>
#include <esp_timer.h>
#include <algorithm>
#include "display_manager.hpp"

esp_err_t display_manager::init()
{
#ifdef CONFIG_SI_DISP_ENABLE
    ESP_LOGI(TAG, "Panel init");
    if (panel == nullptr) {
        ESP_LOGW(TAG, "No display panel configured, display become no-op!!");
        return ESP_OK;
    }

    auto ret = panel->init();
    if (ret != ESP_OK) {
        return ret;
    }

    ESP_LOGI(TAG, "UI task init OK");

    ret = composer.init();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to set up UI composer 0x%x", ret);
        panel->set_backlight(100); // a blank panel beats a dark one that looks dead
        return ret;
    }

    // Light the panel once the first screen is in its memory, not before:
    // until then it shows random pixels. The refresh returns once the last
    // chunk is queued, so wait for one frame's transfer too.
    if (lvgl_port_lock(1000)) {
        lv_refr_now(nullptr);
        lvgl_port_unlock();
    }
    vTaskDelay(pdMS_TO_TICKS(nt279vj_panel::FRAME_SEND_MS));
    ret = panel->set_backlight(100);

    ESP_LOGI(TAG, "Composer init OK");
    return ret;
#else
    ESP_LOGI(TAG, "Display disabled");
    return composer.init();
#endif
}

disp_panel_if *display_manager::get_panel()
{
    return panel;
}

void display_manager::deinit()
{
    // TODO: not sure now
}

si_ui *display_manager::get_composer()
{
    return &composer;
}
