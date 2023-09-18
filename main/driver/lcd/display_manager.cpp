#include <esp_log.h>
#include "display_manager.hpp"

esp_err_t display_manager::init()
{
    auto ret = panel->init();
    if (ret != ESP_OK) {
        return ret;
    }

    disp_buf_a = static_cast<uint8_t *>(heap_caps_malloc(CONFIG_SI_DISP_PANEL_BUFFER_SIZE * sizeof(lv_color_t), MALLOC_CAP_DMA));
    if (disp_buf_a == nullptr) {
        ESP_LOGE(TAG, "Failed to allocate display buffer");
        return ESP_ERR_NO_MEM;
    }

    disp_buf_b = static_cast<uint8_t *>(heap_caps_malloc(CONFIG_SI_DISP_PANEL_BUFFER_SIZE * sizeof(lv_color_t), MALLOC_CAP_DMA));
    if (disp_buf_b == nullptr) {
        ESP_LOGE(TAG, "Failed to allocate display buffer");
        free(disp_buf_a);
        return ESP_ERR_NO_MEM;
    }

    lv_disp_draw_buf_init(&draw_buf, disp_buf_a, disp_buf_b, CONFIG_SI_DISP_PANEL_BUFFER_SIZE);
    static lv_disp_drv_t disp_drv = {};
    lv_disp_drv_init(&disp_drv);
    disp_drv.flush_cb = handle_flush;
    disp_drv.hor_res = (int16_t)panel->get_hor_size();
    disp_drv.ver_res = (int16_t)panel->get_ver_size();
    disp_drv.draw_buf = &draw_buf;
    disp_drv.antialiasing = 1;

    lv_disp_drv_register(&disp_drv);

    return ret;

}

disp_panel_if *display_manager::get_panel()
{
    return panel;
}

void display_manager::handle_flush(lv_disp_drv_t *disp_drv, const lv_area_t *area, lv_color_t *color_p)
{

}

void IRAM_ATTR display_manager::lv_tick_cb(void *arg)
{
    (void) arg;
    lv_tick_inc(1);
}
