#include "ui_commander.hpp"
#include "lcd/display_manager.hpp"

esp_err_t ui_commander::init()
{
    task_queue = display_manager::instance()->get_ui_queue();
    if (task_queue == nullptr) {
        ESP_LOGE(TAG, "LVGL needs to be init first");
        return ESP_ERR_INVALID_STATE;
    }

    return ESP_OK;
}

esp_err_t ui_commander::display_init()
{
    if (task_queue == nullptr) {
        ESP_LOGE(TAG, "LVGL needs to be init first");
        return ESP_ERR_INVALID_STATE;
    }

    ui_state::queue_item item = {};
    item.state = ui_state::STATE_INIT;

    xQueueSend(task_queue, &item, portMAX_DELAY);
    return ESP_OK;
}

esp_err_t ui_commander::display_chip_erase()
{
    ui_state::queue_item item = {};
    item.state = ui_state::STATE_ERASE;

    xQueueSend(task_queue, &item, portMAX_DELAY);
    return ESP_OK;
}

esp_err_t ui_commander::display_flash(ui_state::flash_screen *screen)
{
    ui_state::queue_item item = {};
    item.state = ui_state::STATE_FLASH;
    item.total_count = 100;
    item.percentage = screen->percentage;

    xQueueSend(task_queue, &item, portMAX_DELAY);
    return ESP_OK;
}

esp_err_t ui_commander::display_test(ui_state::test_screen *screen)
{
    ui_state::queue_item item = {};
    item.state = ui_state::STATE_TEST;
    item.total_count = screen->total_test;
    item.percentage = screen->done_test;
    strncpy(item.comment, screen->subtitle, sizeof(ui_state::queue_item::comment));
    item.comment[sizeof(ui_state::queue_item::comment) - 1] = '\0';

    xQueueSend(task_queue, &item, portMAX_DELAY);
    return ESP_OK;
}

esp_err_t ui_commander::display_error(ui_state::error_screen *screen)
{
    ui_state::queue_item item = {};
    item.state = ui_state::STATE_ERROR;
    strncpy(item.comment, screen->subtitle, sizeof(ui_state::queue_item::comment));
    item.comment[sizeof(ui_state::queue_item::comment) - 1] = '\0';

    xQueueSend(task_queue, &item, portMAX_DELAY);
    return ESP_OK;
}

esp_err_t ui_commander::display_done()
{
    ui_state::queue_item item = {};
    item.state = ui_state::STATE_DONE;

    xQueueSend(task_queue, &item, portMAX_DELAY);
    return ESP_OK;
}

esp_err_t ui_commander::display_usb()
{
    ui_state::queue_item item = {};
    item.state = ui_state::STATE_USB;

    xQueueSend(task_queue, &item, portMAX_DELAY);
    return ESP_OK;
}
