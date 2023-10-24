#include "ui_producer_sm.hpp"

esp_err_t ui_producer_sm::init()
{
    producer_queue = xQueueCreate(3, sizeof(ui_state::queue_item));
    if (producer_queue == nullptr) {
        return ESP_ERR_NO_MEM;
    }

    return ESP_OK;
}

esp_err_t ui_producer_sm::deinit()
{
    if (producer_queue == nullptr) {
        return ESP_ERR_INVALID_STATE;
    }

    vQueueDelete(producer_queue);
    return ESP_OK;
}

esp_err_t ui_producer_sm::display(ui_state::init_screen *screen)
{
    return 0;
}

esp_err_t ui_producer_sm::display(ui_state::erase_screen *screen)
{
    return 0;
}

esp_err_t ui_producer_sm::display(ui_state::flash_screen *screen)
{
    return 0;
}

esp_err_t ui_producer_sm::display(ui_state::test_screen *screen)
{
    return 0;
}

esp_err_t ui_producer_sm::display(ui_state::error_screen *screen)
{
    return 0;
}

QueueHandle_t ui_producer_sm::get_queue()
{
    return producer_queue;
}
