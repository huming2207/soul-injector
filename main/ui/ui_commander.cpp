#include "ui_commander.hpp"

esp_err_t ui_commander::init()
{
    return ESP_OK;
}

esp_err_t ui_commander::deinit()
{
    return ESP_OK;
}

esp_err_t ui_commander::display(ui_state::init_screen *screen)
{
    return 0;
}

esp_err_t ui_commander::display(ui_state::erase_screen *screen)
{
    return 0;
}

esp_err_t ui_commander::display(ui_state::flash_screen *screen)
{
    return 0;
}

esp_err_t ui_commander::display(ui_state::test_screen *screen)
{
    return 0;
}

esp_err_t ui_commander::display(ui_state::error_screen *screen)
{
    return 0;
}
