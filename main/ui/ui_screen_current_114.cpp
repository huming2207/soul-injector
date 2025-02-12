#include <lvgl.h>
#include "ui_screen_current_114.hpp"

esp_err_t ui_screen_current_114::init(lv_obj_t *_base_obj)
{
    if (_base_obj == nullptr) {
        return ESP_ERR_INVALID_ARG;
    }

    base_obj = _base_obj;

    header_label = lv_label_create(base_obj);
    if (header_label == nullptr) {
        return ESP_ERR_NO_MEM;
    }

    lv_obj_set_pos(header_label, 0, 0);
    lv_obj_set_size(header_label, 240, LV_SIZE_CONTENT);
    lv_label_set_text(header_label, "Current Test");
    lv_obj_set_style_text_align(header_label, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_text_font(header_label, &lv_font_montserrat_16, LV_PART_MAIN | LV_STATE_DEFAULT);

    current_label = lv_label_create(base_obj);
    if (current_label == nullptr) {
        return ESP_ERR_NO_MEM;
    }

    lv_obj_set_pos(current_label, 0, 20);
    lv_obj_set_size(current_label, 240, 32);
    lv_label_set_text(current_label, "0.000000 uA");
    lv_obj_set_style_text_align(current_label, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_text_font(current_label, &lv_font_montserrat_28, LV_PART_MAIN | LV_STATE_DEFAULT);

    energy_label = lv_label_create(base_obj);
    if (energy_label == nullptr) {
        return ESP_ERR_NO_MEM;
    }

    lv_obj_set_pos(energy_label, 0, 56);
    lv_obj_set_size(energy_label, 240, 31);
    lv_label_set_text(energy_label, "123456.789 mC");
    lv_obj_set_style_text_align(energy_label, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_text_font(energy_label, &lv_font_montserrat_28, LV_PART_MAIN | LV_STATE_DEFAULT);

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

void ui_screen_current_114::set_current(const char *text)
{
    if (current_label == nullptr) {
        return;
    }

    lv_label_set_text(current_label, text);
}

void ui_screen_current_114::set_energy(const char *text)
{
    if (energy_label == nullptr) {
        return;
    }

    lv_label_set_text(energy_label, text);
}

void ui_screen_current_114::set_state(const char *text, lv_color_t bg_color)
{
    if (state_label == nullptr) {
        return;
    }

    lv_label_set_text(state_label, text);
    lv_obj_set_style_bg_color(state_label, bg_color, LV_PART_MAIN | LV_STATE_DEFAULT);
}
