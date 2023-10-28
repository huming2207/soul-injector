#include <esp_err.h>
#include <lvgl.h>
#include "ui_composer_114.hpp"
#include "lcd/display_manager.hpp"

esp_err_t ui_composer_114::init()
{
    disp_obj = lv_disp_get_scr_act(display_manager::instance()->get_panel()->get_lv_disp());
    if (disp_obj == nullptr) {
        ESP_LOGE(TAG, "LVGL needs to be init first");
        return ESP_ERR_INVALID_STATE;
    }

    recreate_widget();
    return ESP_OK;
}

esp_err_t ui_composer_114::wait_and_draw()
{
    QueueHandle_t task_queue = display_manager::instance()->get_ui_queue();
    if (task_queue == nullptr) {
        ESP_LOGE(TAG, "UI queue needs to be init first");
        return ESP_ERR_INVALID_STATE;
    }

    ui_state::queue_item item = {};
    if (xQueueReceive(task_queue, &item, portMAX_DELAY) != pdTRUE) {
        ESP_LOGW(TAG, "Failed to receive from task queue");
        return ESP_OK;
    }

    switch (item.state) {
        case ui_state::STATE_EMPTY: {
            ESP_LOGW(TAG, "Ignore unsupported STATE_EMPTY");
            return ESP_ERR_INVALID_STATE;
        }

        case ui_state::STATE_INIT: {
            return draw_init(&item);
        }

        case ui_state::STATE_ERASE: {
            return draw_erase(&item);
        }

        case ui_state::STATE_FLASH: {
            return draw_flash(&item);
        }

        case ui_state::STATE_TEST: {
            return draw_test(&item);
        }

        case ui_state::STATE_ERROR: {
            return draw_error(&item);
        }

        case ui_state::STATE_DONE: {
            return draw_done(&item);
        }

        case ui_state::STATE_USB: {
            return draw_usb(&item);
        }
    }

    return ESP_OK;
}

esp_err_t ui_composer_114::draw_init(ui_state::queue_item *screen)
{
    if (disp_obj == nullptr) {
        ESP_LOGE(TAG, "LVGL needs to be init first");
        return ESP_ERR_INVALID_STATE;
    }

    if (curr_state != ui_state::STATE_INIT) {
        recreate_widget(true);
        curr_state = ui_state::STATE_INIT;
    }

    lv_label_set_text(top_label, "INIT");
    lv_label_set_text(bottom_label, LV_SYMBOL_REFRESH);
    lv_label_set_text(bottom_comment, screen->comment);
    lv_obj_set_style_bg_color(bottom_sect, lv_color_make(0, 0, 220), 0); // Not-so-bright blue
    return ESP_OK;
}

esp_err_t ui_composer_114::draw_erase(ui_state::queue_item *screen)
{
    if (disp_obj == nullptr) {
        ESP_LOGE(TAG, "LVGL needs to be init first");
        return ESP_ERR_INVALID_STATE;
    }

    if (curr_state != ui_state::STATE_ERASE) {
        recreate_widget();
        curr_state = ui_state::STATE_ERASE;
    }

    lv_label_set_text(top_label, "ERASE");
    lv_label_set_text(bottom_label, LV_SYMBOL_REFRESH);
    lv_obj_set_style_bg_color(bottom_sect, lv_color_make(0, 0, 220), 0); // Not-so-bright blue
    return ESP_OK;
}

esp_err_t ui_composer_114::draw_flash(ui_state::queue_item *screen)
{
    if (disp_obj == nullptr) {
        ESP_LOGE(TAG, "LVGL needs to be init first");
        return ESP_ERR_INVALID_STATE;
    }

    if (curr_state != ui_state::STATE_FLASH) {
        recreate_widget();
        curr_state = ui_state::STATE_FLASH;
    }

    lv_label_set_text(top_label, "FLASH");
    lv_label_set_text_fmt(bottom_label, "%d%%", screen->percentage);
    lv_obj_set_style_bg_color(bottom_sect, lv_color_make(226, 220, 0), 0); // Dark ish yellow
    return ESP_OK;
}

esp_err_t ui_composer_114::draw_test(ui_state::queue_item *screen)
{
    if (disp_obj == nullptr) {
        ESP_LOGE(TAG, "LVGL needs to be init first");
        return ESP_ERR_INVALID_STATE;
    }

    if (curr_state != ui_state::STATE_TEST) {
        recreate_widget(true);
        curr_state = ui_state::STATE_TEST;
    }

    lv_label_set_text(top_label, "TEST");
    lv_label_set_text_fmt(bottom_label, "%u/%u", screen->percentage, screen->total_count);
    lv_label_set_text(bottom_comment, screen->comment);
    lv_obj_set_style_bg_color(bottom_sect, lv_color_make(128, 0, 128), 0); // Dark purple
    return ESP_OK;
}

esp_err_t ui_composer_114::draw_error(ui_state::queue_item *screen)
{
    if (disp_obj == nullptr) {
        ESP_LOGE(TAG, "LVGL needs to be init first");
        return ESP_ERR_INVALID_STATE;
    }

    if (curr_state != ui_state::STATE_ERROR) {
        recreate_widget();
        curr_state = ui_state::STATE_ERROR;
    }

    lv_label_set_text(top_label, "TEST");
    lv_label_set_text(bottom_label, LV_SYMBOL_CLOSE);
    lv_label_set_text(bottom_comment, screen->comment);
    lv_obj_set_style_bg_color(bottom_sect, lv_color_make(230, 0, 0), 0); // Not-so-bright red

    return ESP_OK;
}


esp_err_t ui_composer_114::draw_done(ui_state::queue_item *screen)
{
    if (disp_obj == nullptr) {
        ESP_LOGE(TAG, "LVGL needs to be init first");
        return ESP_ERR_INVALID_STATE;
    }

    if (curr_state != ui_state::STATE_ERROR) {
        recreate_widget();
        lv_label_set_text(top_label, "USB");
        lv_obj_set_style_bg_color(bottom_sect, lv_color_make(0, 200, 0), 0); // Dark-ish green
        curr_state = ui_state::STATE_ERROR;
    }

    return ESP_OK;
}

esp_err_t ui_composer_114::draw_usb(ui_state::queue_item *screen)
{
    if (disp_obj == nullptr) {
        ESP_LOGE(TAG, "LVGL needs to be init first");
        return ESP_ERR_INVALID_STATE;
    }

    if (curr_state != ui_state::STATE_ERROR) {
        recreate_widget();
        lv_obj_set_style_bg_color(bottom_sect, lv_color_make(255, 117, 23), 0); // Orange
        curr_state = ui_state::STATE_ERROR;
    }

    return ESP_OK;
}

esp_err_t ui_composer_114::recreate_widget(bool with_comment)
{
    if (base_obj != nullptr) {
        lv_obj_del(base_obj);
    }

    base_obj = lv_obj_create(disp_obj);
    lv_obj_set_style_radius(base_obj, 0, 0);
    lv_obj_set_scrollbar_mode(base_obj, LV_SCROLLBAR_MODE_OFF);
    lv_obj_set_style_pad_all(base_obj, 0, 0);
    lv_obj_set_style_border_width(base_obj, 0, 0);
    lv_obj_set_size(base_obj, (int16_t)display_manager::instance()->get_panel()->get_hor_size(), (int16_t)display_manager::instance()->get_panel()->get_ver_size());
    lv_obj_set_pos(base_obj, 0, 0);
    lv_obj_set_align(base_obj, LV_ALIGN_TOP_LEFT);
    lv_obj_set_style_bg_color(base_obj, lv_color_white(), 0);

    top_sect = lv_obj_create(base_obj);
    lv_obj_set_style_radius(top_sect, 0, 0);
    lv_obj_set_scrollbar_mode(top_sect, LV_SCROLLBAR_MODE_OFF);
    lv_obj_set_style_pad_all(top_sect, 0, 0);
    lv_obj_set_style_border_width(top_sect, 0, 0);
    lv_obj_set_size(top_sect, (int16_t)display_manager::instance()->get_panel()->get_hor_size(), 100);
    lv_obj_set_pos(top_sect, 0, 0);
    lv_obj_set_align(top_sect, LV_ALIGN_TOP_LEFT);
    lv_obj_set_style_bg_color(top_sect, lv_color_white(), 0);

    top_label = lv_label_create(top_sect);
    lv_obj_set_style_text_align(top_label, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_font(top_label, &lv_font_montserrat_30, 0);
    lv_obj_set_align(top_label, LV_ALIGN_CENTER);

    bottom_sect = lv_obj_create(base_obj);
    lv_obj_set_style_radius(bottom_sect, 0, 0);
    lv_obj_set_scrollbar_mode(bottom_sect, LV_SCROLLBAR_MODE_OFF);
    lv_obj_set_style_pad_all(bottom_sect, 0, 0);
    lv_obj_set_style_border_width(bottom_sect, 0, 0);
    lv_obj_set_size(bottom_sect, (int16_t)display_manager::instance()->get_panel()->get_hor_size(), 140);
    lv_obj_set_pos(bottom_sect, 0, 100);
    lv_obj_set_align(bottom_sect, LV_ALIGN_TOP_LEFT);
    lv_obj_set_style_bg_color(bottom_sect, lv_color_make(255, 117, 23), 0);

    bottom_label = lv_label_create(bottom_sect);
    lv_label_set_text(bottom_label, LV_SYMBOL_OK);
    lv_obj_set_style_text_font(bottom_label, &lv_font_montserrat_36, 0);
    lv_obj_set_align(bottom_label, LV_ALIGN_CENTER);
    lv_obj_set_style_text_color(bottom_label, lv_color_white(), 0);

    if (!with_comment) {
        lv_obj_set_style_text_align(bottom_label, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_set_align(bottom_label, LV_ALIGN_CENTER);
    } else {
        lv_obj_set_style_text_align(bottom_label, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_set_align(bottom_label, LV_ALIGN_CENTER);
        lv_obj_set_pos(bottom_label, 0, -20);

        bottom_comment = lv_label_create(bottom_sect);
        lv_obj_set_style_text_font(bottom_comment, &lv_font_montserrat_20, 0);
        lv_obj_set_style_text_color(bottom_comment, lv_color_white(), 0);
        lv_obj_set_align(bottom_comment, LV_ALIGN_CENTER);
        lv_obj_set_pos(bottom_comment, 0, 20);
    }

    return ESP_OK;
}
