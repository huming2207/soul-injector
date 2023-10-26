#include <esp_err.h>
#include <lvgl.h>
#include "ui_sm_114.hpp"
#include "lcd/display_manager.hpp"

esp_err_t ui_composer_114::init()
{
//    ui_queue = ui_commander::instance()->get_queue();
//    if (ui_queue == nullptr) {
//        ESP_LOGE(TAG, "UI queue needs to be init first");
//        return ESP_ERR_INVALID_STATE;
//    }

    disp_obj = lv_disp_get_scr_act(display_manager::instance()->get_panel()->get_lv_disp());
    if (disp_obj == nullptr) {
        ESP_LOGE(TAG, "LVGL needs to be init first");
        return ESP_ERR_INVALID_STATE;
    }

    recreate_widget(true);
    return ESP_OK;
}

esp_err_t ui_composer_114::deinit()
{
    return ESP_OK;
}

esp_err_t ui_composer_114::draw_init(ui_state::queue_item *screen)
{
    if (disp_obj == nullptr) {
        ESP_LOGE(TAG, "LVGL needs to be init first");
        return ESP_ERR_INVALID_STATE;
    }

    if (curr_state != ui_state::STATE_INIT) {
        recreate_widget();
        curr_state = ui_state::STATE_INIT;
    }


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
        lv_obj_set_style_bg_color(bottom_sect, lv_color_make(226, 220, 0), 0); // Dark ish yellow
        curr_state = ui_state::STATE_FLASH;
    }

    return ESP_OK;
}

esp_err_t ui_composer_114::draw_test(ui_state::queue_item *screen)
{
    if (disp_obj == nullptr) {
        ESP_LOGE(TAG, "LVGL needs to be init first");
        return ESP_ERR_INVALID_STATE;
    }

    if (curr_state != ui_state::STATE_TEST) {
        recreate_widget();
        lv_obj_set_style_bg_color(bottom_sect, lv_color_make(128, 0, 128), 0); // Dark purple
        curr_state = ui_state::STATE_TEST;
    }

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
        lv_obj_set_style_bg_color(bottom_sect, lv_color_make(230, 0, 0), 0); // Not-so-bright red
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
    lv_label_set_text(top_label, "FLASH");
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
    lv_obj_set_style_bg_color(bottom_sect, lv_color_make(226, 220, 0), 0);

    bottom_label = lv_label_create(bottom_sect);
    lv_label_set_text(bottom_label, "100%");
    lv_obj_set_style_text_font(bottom_label, &lv_font_montserrat_32, 0);
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
