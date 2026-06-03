#include "ui_composer_noop.hpp"

esp_err_t ui_composer_noop::init()
{
    return ESP_OK;
}

esp_err_t ui_composer_noop::display_init()
{
    return ESP_OK;
}

esp_err_t ui_composer_noop::display_erase(uint8_t percentage)
{
    (void)percentage;
    return ESP_OK;
}

esp_err_t ui_composer_noop::display_test(size_t done, size_t total, const char *test_msg)
{
    (void)done;
    (void)total;
    (void)test_msg;
    return ESP_OK;
}

esp_err_t ui_composer_noop::display_program(uint8_t percentage)
{
    (void)percentage;
    return ESP_OK;
}

esp_err_t ui_composer_noop::display_done()
{
    return ESP_OK;
}

esp_err_t ui_composer_noop::display_error(const char *header, const char *err_msg)
{
    (void)header;
    (void)err_msg;
    return ESP_OK;
}

esp_err_t ui_composer_noop::display_config()
{
    return ESP_OK;
}

esp_err_t ui_composer_noop::display_current(double min_ua, double max_ua, double avg_ua, const char *state, lv_color_t state_color)
{
    (void)min_ua;
    (void)max_ua;
    (void)avg_ua;
    (void)state;
    (void)state_color;
    return ESP_OK;
}
