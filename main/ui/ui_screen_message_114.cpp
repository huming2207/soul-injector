#include <lvgl.h>
#include "ui_screen_message_114.hpp"

esp_err_t ui_screen_message_114::init(lv_obj_t *_base_obj)
{
    if (_base_obj == nullptr) {
        return ESP_ERR_INVALID_ARG;
    }

    base_obj = _base_obj;

    lv_obj_set_pos(base_obj, 0, 0);
    lv_obj_set_size(base_obj, 240, 135);
    lv_obj_set_style_bg_color(base_obj, lv_color_hex(0xff000000), LV_PART_MAIN | LV_STATE_DEFAULT);

    comment_label = lv_label_create(progress_bar);
    if (comment_label == nullptr) {
        return ESP_ERR_NO_MEM;
    }

    lv_obj_set_pos(comment_label, 0, 68);
    lv_obj_set_size(comment_label, 240, 50);
    lv_obj_set_size(comment_label, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_label_set_text(comment_label, "0%");
    lv_obj_set_style_text_color(comment_label, lv_color_hex(0xff000000), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_text_font(comment_label, &lv_font_montserrat_16, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_text_align(comment_label, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN | LV_STATE_DEFAULT);

    header_label = lv_label_create(progress_bar);
    if (header_label == nullptr) {
        return ESP_ERR_NO_MEM;
    }

    lv_obj_set_pos(header_label, 0, -25);
    lv_obj_set_size(header_label, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_label_set_recolor(header_label, true);
    lv_label_set_text(header_label, "UNKNOWN");
    lv_obj_set_style_align(header_label, LV_ALIGN_CENTER, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_text_font(header_label, &lv_font_montserrat_26, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_text_color(header_label, lv_color_hex(0xff000000), LV_PART_MAIN | LV_STATE_DEFAULT);

    return ESP_OK;
}

void ui_screen_message_114::deinit()
{
    if (base_obj != nullptr) {
        lv_obj_del(base_obj);
    }
}

void ui_screen_message_114::set_header_text(const char *text)
{
    if (text == nullptr || header_label == nullptr) {
        return;
    }

    lv_label_set_text(header_label, text);
}

void ui_screen_message_114::set_comment_text(const char *text)
{
    if (text == nullptr || comment_label == nullptr) {
        return;
    }

    lv_label_set_text(comment_label, text);
}

void ui_screen_message_114::set_color(lv_color_t background, lv_color_t text)
{
    if (comment_label == nullptr || header_label == nullptr || base_obj == nullptr) {
        return;
    }

    lv_obj_set_style_bg_color(base_obj, background, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_text_color(header_label, text, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_text_color(comment_label, text, LV_PART_MAIN | LV_STATE_DEFAULT);
}
