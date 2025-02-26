#include <lvgl.h>
#include "ui_screen_current_114.hpp"

esp_err_t ui_screen_current_114::init(lv_obj_t *_base_obj)
{
    if (_base_obj == nullptr) {
        return ESP_ERR_INVALID_ARG;
    }

    base_obj = _base_obj;

    lv_obj_set_style_bg_color(base_obj, lv_color_white(), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_opa(base_obj, 255, LV_PART_MAIN | LV_STATE_DEFAULT);
    header_label = lv_label_create(base_obj);
    if (header_label == nullptr) {
        return ESP_ERR_NO_MEM;
    }

    lv_obj_set_pos(header_label, 0, 0);
    lv_obj_set_size(header_label, 240, 36);
    lv_label_set_text(header_label, "Current Test");
    lv_obj_set_style_text_align(header_label, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_text_font(header_label, &lv_font_montserrat_32, LV_PART_MAIN | LV_STATE_DEFAULT);

    current_label = lv_label_create(base_obj);
    if (current_label == nullptr) {
        return ESP_ERR_NO_MEM;
    }

    lv_obj_set_pos(current_label, 0, 40);
    lv_obj_set_size(current_label, 240, 40);
    lv_label_set_text(current_label, "0.000000 uA");
    lv_obj_set_style_text_align(current_label, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_text_font(current_label, &lv_font_montserrat_12, LV_PART_MAIN | LV_STATE_DEFAULT);

    state_label = lv_label_create(base_obj);
    if (state_label == nullptr) {
        return ESP_ERR_NO_MEM;
    }

    lv_obj_set_pos(state_label, 0, 87);
    lv_obj_set_size(state_label, 240, 48);
    lv_label_set_text(state_label, "PENDING");
    lv_obj_set_style_text_align(state_label, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_text_font(state_label, &lv_font_montserrat_48, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_color(state_label, lv_color_hex(0xffffff00), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_opa(state_label, 255, LV_PART_MAIN | LV_STATE_DEFAULT);

    return ESP_OK;
}

void ui_screen_current_114::deinit()
{
    if (base_obj != nullptr) {
        lv_obj_del(base_obj);
    }
}

void ui_screen_current_114::set_current_main(const char *text)
{
    if (current_label == nullptr) {
        return;
    }

    lv_label_set_text(current_label, text);
}

void ui_screen_current_114::set_state(const char *text, lv_color_t bg_color)
{
    if (state_label == nullptr) {
        return;
    }

    lv_label_set_text(state_label, text);
    lv_obj_set_style_bg_color(state_label, bg_color, LV_PART_MAIN | LV_STATE_DEFAULT);
}
