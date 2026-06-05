#include <lvgl.h>
#include "ui_screen_progress_114.hpp"

esp_err_t ui_screen_progress_114::init(lv_obj_t *_base_obj)
{
    if (_base_obj == nullptr) {
        return ESP_ERR_INVALID_ARG;
    }

    base_obj = _base_obj;

    progress_bar = lv_bar_create(base_obj);
    if (progress_bar == nullptr) {
        return ESP_ERR_NO_MEM;
    }

    lv_obj_set_pos(progress_bar, 0, 0);
    lv_obj_set_size(progress_bar, 240, 135);
    lv_bar_set_value(progress_bar, 25, LV_ANIM_OFF);
    lv_obj_set_style_radius(progress_bar, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_color(progress_bar, lv_color_hex(0xffffffff), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_opa(progress_bar, 255, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_radius(progress_bar, 0, LV_PART_INDICATOR | LV_STATE_DEFAULT);

    header_label = lv_label_create(progress_bar);
    if (header_label == nullptr) {
        return ESP_ERR_NO_MEM;
    }

    lv_obj_set_pos(header_label, 0, 16);
    lv_obj_set_size(header_label, 240, 36);
    lv_label_set_recolor(header_label, true);
    lv_label_set_text(header_label, "UNKNOWN");
    lv_obj_set_style_align(header_label, LV_ALIGN_TOP_LEFT, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_text_font(header_label, &lv_font_montserrat_28, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_text_color(header_label, lv_color_hex(0xff000000), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_text_align(header_label, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN | LV_STATE_DEFAULT);

    comment_label = lv_label_create(progress_bar);
    if (comment_label == nullptr) {
        return ESP_ERR_NO_MEM;
    }

    lv_obj_set_pos(comment_label, 0, 53);
    lv_obj_set_size(comment_label, 240, 82);
    lv_label_set_text(comment_label, "0%");
    lv_obj_set_style_align(header_label, LV_ALIGN_TOP_LEFT, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_text_color(comment_label, lv_color_hex(0xff000000), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_text_font(comment_label, &lv_font_montserrat_16, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_text_align(comment_label, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN | LV_STATE_DEFAULT);

    return ESP_OK;
}

void ui_screen_progress_114::deinit()
{
    if (base_obj != nullptr) {
        lv_obj_del(base_obj);
    }
}

void ui_screen_progress_114::set_progress(uint32_t done, uint32_t full)
{
    if (progress_bar == nullptr) {
        return;
    }

    lv_bar_set_range(progress_bar, 0, (int32_t)full);
    lv_bar_set_value(progress_bar, (int32_t)done, LV_ANIM_OFF);
}

void ui_screen_progress_114::set_header_text(const char *text)
{
    if (text == nullptr || header_label == nullptr) {
        return;
    }

    lv_label_set_text(header_label, text);
}

void ui_screen_progress_114::set_comment_text(const char *text)
{
    if (text == nullptr || comment_label == nullptr) {
        return;
    }

    lv_label_set_text(comment_label, text);
}

void ui_screen_progress_114::set_progress_bar_color(lv_color_t indicator_color, lv_color_t bg_color )
{
    if (progress_bar == nullptr) {
        return;
    }

    lv_obj_set_style_bg_color(progress_bar, indicator_color, LV_PART_INDICATOR | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_color(progress_bar, bg_color, LV_PART_MAIN | LV_STATE_DEFAULT);
}
