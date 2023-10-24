#include <esp_err.h>
#include <lvgl.h>
#include "ui_sm_114.hpp"

esp_err_t ui_consumer_sm_114::init()
{
    ui_queue = ui_producer_sm::instance()->get_queue();
    if (ui_queue != nullptr) {
        ESP_LOGE(TAG, "UI queue needs to be init first");
        return ESP_ERR_INVALID_STATE;
    }

    return ESP_OK;
}

esp_err_t ui_consumer_sm_114::deinit()
{
    return ESP_OK;
}

esp_err_t ui_consumer_sm_114::draw_init(ui_state::queue_item *screen)
{


    return ESP_OK;
}

esp_err_t ui_consumer_sm_114::draw_erase(ui_state::queue_item *screen)
{
    return ESP_OK;
}

esp_err_t ui_consumer_sm_114::draw_flash(ui_state::queue_item *screen)
{
    return ESP_OK;
}

esp_err_t ui_consumer_sm_114::draw_test(ui_state::queue_item *screen)
{
    return ESP_OK;
}

esp_err_t ui_consumer_sm_114::draw_error(ui_state::queue_item *screen)
{
    return ESP_OK;
}
