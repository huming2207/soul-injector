#include <freertos/FreeRTOS.h>
#include <freertos/event_groups.h>

#include "ui_composer_114.hpp"

esp_err_t ui_composer_114::display_init()
{
    if (wait_for_ui_mod() != ESP_OK) {
        return ESP_ERR_TIMEOUT;
    }

    if (screen_state != ui_screen::MESSAGE) {
        reload_base_obj();
        auto ret = msg_screen.init(base_obj);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "Can't setup message screen");
            return ESP_ERR_NO_MEM;
        } else {
            screen_state = ui_screen::MESSAGE;
        }
    }

    msg_screen.set_header_text("READY");
    msg_screen.set_comment_text("Please connect target");
    msg_screen.set_color(lv_color_white(), lv_color_black());

    set_ready();
    return ESP_OK;
}

esp_err_t ui_composer_114::display_erase(uint8_t percentage)
{
    if (wait_for_ui_mod() != ESP_OK) {
        return ESP_ERR_TIMEOUT;
    }

    if (percentage == UINT8_MAX) {
        if (screen_state != ui_screen::MESSAGE) {
            reload_base_obj();
            auto ret = msg_screen.init(base_obj);
            if (ret != ESP_OK) {
                ESP_LOGE(TAG, "Can't setup message screen");
                return ESP_ERR_NO_MEM;
            } else {
                screen_state = ui_screen::MESSAGE;
            }
        }

        msg_screen.set_header_text("ERASING");
        msg_screen.set_comment_text("Full chip erase");
        msg_screen.set_color(lv_color_make(0x7a, 0xff, 0xff), lv_color_black()); // Light cyan bar + black text

        set_ready();
        return ESP_OK;
    } else {
        if (screen_state != ui_screen::PROGRESS) {
            reload_base_obj();
            auto ret = progress_screen.init(base_obj);
            if (ret != ESP_OK) {
                ESP_LOGE(TAG, "Can't setup message screen");
                return ESP_ERR_NO_MEM;
            } else {
                screen_state = ui_screen::PROGRESS;
                progress_screen.set_header_text("ERASING");
                progress_screen.set_progress_bar_color(lv_color_make(0x7a, 0xff, 0xff), lv_color_white()); // Light cyan bar + white
            }
        }

        char comment[32] = { 0 };
        snprintf(comment, sizeof(comment), "%03u %%", percentage);
        progress_screen.set_comment_text(comment);
        progress_screen.set_progress(percentage, 100);

        set_ready();
        return ESP_OK;
    }
}

esp_err_t ui_composer_114::display_test(uint8_t percentage, const char *test_msg)
{
    if (wait_for_ui_mod() != ESP_OK) {
        return ESP_ERR_TIMEOUT;
    }

    if (screen_state != ui_screen::PROGRESS) {
        reload_base_obj();
        auto ret = progress_screen.init(base_obj);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "Can't setup message screen");
            return ESP_ERR_NO_MEM;
        } else {
            screen_state = ui_screen::PROGRESS;
            progress_screen.set_header_text("TESTING");
            progress_screen.set_progress_bar_color(lv_color_make(0xcb, 0xc3, 0xe3), lv_color_white()); // Light purple + white
        }
    }

    if (test_msg == nullptr) {
        char comment[32] = { 0 };
        snprintf(comment, sizeof(comment), "%03u %%", percentage);
        progress_screen.set_comment_text(comment);
    } else {
        progress_screen.set_comment_text(test_msg);
    }

    progress_screen.set_progress(percentage, 100);
    set_ready();
    return ESP_OK;
}

esp_err_t ui_composer_114::display_program(uint8_t percentage)
{
    if (wait_for_ui_mod() != ESP_OK) {
        return ESP_ERR_TIMEOUT;
    }

    if (percentage == UINT8_MAX) {
        if (screen_state != ui_screen::MESSAGE) {
            reload_base_obj();
            auto ret = msg_screen.init(base_obj);
            if (ret != ESP_OK) {
                ESP_LOGE(TAG, "Can't setup message screen");
                return ESP_ERR_NO_MEM;
            } else {
                screen_state = ui_screen::MESSAGE;
            }
        }

        msg_screen.set_header_text("PROGRAMMING");
        msg_screen.set_comment_text("Just wait");
        msg_screen.set_color(lv_color_make(255, 255, 0), lv_color_black());

        set_ready();
        return ESP_OK;
    } else {
        if (screen_state != ui_screen::PROGRESS) {
            reload_base_obj();
            auto ret = progress_screen.init(base_obj);
            if (ret != ESP_OK) {
                ESP_LOGE(TAG, "Can't setup message screen");
                return ESP_ERR_NO_MEM;
            } else {
                screen_state = ui_screen::PROGRESS;
                progress_screen.set_header_text("PROGRAMMING");
                progress_screen.set_progress_bar_color(lv_color_make(255, 255, 0), lv_color_white());
            }
        }

        char comment[32] = { 0 };
        snprintf(comment, sizeof(comment), "%03u %%", percentage);
        progress_screen.set_comment_text(comment);
        progress_screen.set_progress(percentage, 100);

        set_ready();
        return ESP_OK;
    }
}

esp_err_t ui_composer_114::display_done()
{
    if (wait_for_ui_mod() != ESP_OK) {
        return ESP_ERR_TIMEOUT;
    }

    if (screen_state != ui_screen::MESSAGE) {
        reload_base_obj();
        auto ret = msg_screen.init(base_obj);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "Can't setup message screen");
            return ESP_ERR_NO_MEM;
        } else {
            screen_state = ui_screen::MESSAGE;
        }
    }

    msg_screen.set_header_text("DONE");
    msg_screen.set_comment_text("Move to next one");
    msg_screen.set_color(lv_color_make(0x88, 0xe7, 0x88), lv_color_black());

    set_ready();
    return ESP_OK;
}

esp_err_t ui_composer_114::display_error(const char *header, const char *err_msg)
{
    if (wait_for_ui_mod() != ESP_OK) {
        return ESP_ERR_TIMEOUT;
    }

    if (screen_state != ui_screen::MESSAGE) {
        reload_base_obj();
        auto ret = msg_screen.init(base_obj);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "Can't setup message screen");
            return ESP_ERR_NO_MEM;
        } else {
            screen_state = ui_screen::MESSAGE;
        }
    }

    msg_screen.set_header_text("DONE");
    msg_screen.set_comment_text("Move to next one");
    msg_screen.set_color(lv_color_make(0x88, 0xe7, 0x88), lv_color_black());

    set_ready();
    return ESP_OK;
}

esp_err_t ui_composer_114::display_config()
{
    if (wait_for_ui_mod() != ESP_OK) {
        return ESP_ERR_TIMEOUT;
    }

    if (screen_state != ui_screen::MESSAGE) {
        reload_base_obj();
        auto ret = msg_screen.init(base_obj);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "Can't setup message screen");
            return ESP_ERR_NO_MEM;
        } else {
            screen_state = ui_screen::MESSAGE;
        }
    }

    msg_screen.set_header_text("CONFIG");
    msg_screen.set_comment_text("Connect me to USB");
    msg_screen.set_color(lv_color_white(), lv_color_black());

    set_ready();
    return ESP_OK;
}

esp_err_t ui_composer_114::display_current(float current_ua, float energy_mc, const char *state, lv_color_t state_color)
{
    if (wait_for_ui_mod() != ESP_OK) {
        return ESP_ERR_TIMEOUT;
    }

    if (screen_state != ui_screen::CURRENT) {
        reload_base_obj();
        auto ret = current_screen.init(base_obj);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "Can't setup message screen");
            return ESP_ERR_NO_MEM;
        } else {
            screen_state = ui_screen::CURRENT;
        }
    }

    char i_reading[32] = { 0 };
    snprintf(i_reading, sizeof(i_reading), "%.3f uA", current_ua);
    current_screen.set_current(i_reading);

    char e_reading[32] = { 0 };
    snprintf(e_reading, sizeof(e_reading), "%.3f mC", energy_mc);
    current_screen.set_energy(e_reading);
    current_screen.set_state(state, state_color);

    set_ready();
    return ESP_OK;
}

void ui_composer_114::wait_and_render()
{
    xEventGroupWaitBits(evt_group, BIT_READY, pdTRUE, pdFALSE, portMAX_DELAY);
    xEventGroupClearBits(evt_group, BIT_NOT_RENDERING);
}

esp_err_t ui_composer_114::init()
{
    evt_group = xEventGroupCreate();
    if (evt_group == nullptr) {
        ESP_LOGE(TAG, "Can't create evt group");
        return ESP_ERR_NO_MEM;
    }

    return ESP_OK;
}

void ui_composer_114::reload_base_obj()
{
    xEventGroupClearBits(evt_group, BIT_READY);
    if (base_obj != nullptr) {
        lv_obj_del(base_obj);
        base_obj = nullptr;
    }

    base_obj = lv_obj_create(lv_scr_act());
}

void ui_composer_114::set_ready() const
{
    xEventGroupSetBits(evt_group, BIT_READY);
}

void ui_composer_114::clear_ready() const
{
    xEventGroupClearBits(evt_group, BIT_READY);
}

void ui_composer_114::render_done()
{
    xEventGroupSetBits(evt_group, BIT_NOT_RENDERING);
}

esp_err_t ui_composer_114::wait_for_ui_mod(uint32_t wait_ticks) const
{
    EventBits_t ret = xEventGroupWaitBits(evt_group, BIT_NOT_RENDERING, pdFALSE, pdFALSE, wait_ticks);
    if ((ret & BIT_NOT_RENDERING) == 0) {
        return ESP_ERR_TIMEOUT;
    }

    return ESP_OK;
}
