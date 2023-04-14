#include <esp_err.h>
#include "lcd_manager.hpp"
#include "lvgl_wrapper.h"

esp_err_t lcd_manager::init()
{
    auto ret = lvgl_disp_init();

    if (ret == ESP_OK) {
        curr_state = lcd::STATE_SPLASH;
        return display_done();
    }

    return ret;
}

esp_err_t lcd_manager::display_splash()
{
    auto ret = clear_display();
    if (ret != ESP_OK) {
        return ret;
    }

    ret = lvgl_take_lock(pdMS_TO_TICKS(1000));
    if (curr_state != lcd::STATE_SPLASH || root_obj == nullptr) {
        ret = ret ?: draw_two_bars(&top_bar, &bottom_bar, lv_color_hex(0x27632a), lv_color_hex(0xff9800));
        curr_state = lcd::STATE_SPLASH;
    } else {
        lvgl_give_lock();
        return ESP_OK;
    }

    auto *top_text = lv_label_create(top_bar);
    lv_obj_align(top_text, LV_ALIGN_CENTER, 0, 0);
    lv_obj_set_style_text_align(top_text, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_text(top_text, "Soul Injector\nFirmware Programmer");
    lv_obj_set_style_text_font(top_text, &lv_font_montserrat_18, 0);
    lv_obj_set_style_text_color(top_text, lv_color_white(), 0);

    auto *bottom_text = lv_label_create(bottom_bar);
    lv_obj_align(bottom_text, LV_ALIGN_TOP_LEFT, 10, 2);
    lv_obj_set_style_text_align(bottom_text, LV_TEXT_ALIGN_LEFT, 0);
    lv_label_set_text_fmt(bottom_text, "By Jackson M Hu\nCopyright (C) 2023\nNO COMMERCIAL USE\nUNLESS APPROVED BY AUTHOR\n\nModel: %s\nVersion: %s\nSDK: %s", CONFIG_SI_DEVICE_MODEL, CONFIG_SI_DEVICE_BUILD, IDF_VER);
    lv_obj_set_style_text_font(bottom_text, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(bottom_text, lv_color_black(), 0);

    lvgl_give_lock();
    return ret;
}

esp_err_t lcd_manager::display_detect()
{
    auto ret = clear_display();
    if (ret != ESP_OK) {
        return ret;
    }

    ret = lvgl_take_lock(pdMS_TO_TICKS(1000));
    if (curr_state != lcd::STATE_DETECT || root_obj == nullptr) {
        ret = ret ?: draw_two_bars(&top_bar, &bottom_bar, lv_color_hex(0x005f56), lv_color_hex(0xb26500)); // Dark cyan
        curr_state = lcd::STATE_DETECT;
    } else {
        lvgl_give_lock();
        return ESP_OK;
    }

    auto *top_text = lv_label_create(top_bar);
    lv_obj_align(top_text, LV_ALIGN_CENTER, 0, 0);
    lv_obj_set_style_text_align(top_text, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_text(top_text, "Detecting");
    lv_obj_set_style_text_font(top_text, &lv_font_montserrat_32, 0);
    lv_obj_set_style_text_color(top_text, lv_color_white(), 0);

    auto *bottom_text = lv_label_create(bottom_bar);
    lv_obj_align(bottom_text, LV_ALIGN_CENTER, 0,16);
    lv_obj_set_style_text_align(bottom_text, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_size(bottom_text, 200, 100);
    lv_label_set_long_mode(bottom_text, LV_LABEL_LONG_WRAP);
    lv_label_set_text(bottom_text, "Please connect pogo pin to target product");
    lv_obj_set_style_text_font(bottom_text, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_color(bottom_text, lv_color_black(), 0);

    lvgl_give_lock();
    return ret;
}

esp_err_t lcd_manager::display_erase()
{
    auto ret = clear_display();
    if (ret != ESP_OK) {
        return ret;
    }

    ret = lvgl_take_lock(pdMS_TO_TICKS(1000));
    if (curr_state != lcd::STATE_ERASE || root_obj == nullptr) {
        ret = ret ?: draw_two_bars(&top_bar, &bottom_bar, lv_color_hex(0x01579b), lv_color_hex(0x631976)); // Dark blue
        curr_state = lcd::STATE_ERASE;
    } else {
        lvgl_give_lock();
        return ESP_OK;
    }

    auto *top_text = lv_label_create(top_bar);
    lv_obj_align(top_text, LV_ALIGN_CENTER, 0, 0);
    lv_obj_set_style_text_align(top_text, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_text(top_text, "Erasing");
    lv_obj_set_style_text_font(top_text, &lv_font_montserrat_32, 0);
    lv_obj_set_style_text_color(top_text, lv_color_white(), 0);

    auto *bottom_text = lv_label_create(bottom_bar);
    lv_obj_align(bottom_text, LV_ALIGN_CENTER, 0,10);
    lv_obj_set_style_text_align(bottom_text, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_size(bottom_text, 200, 100);
    lv_label_set_long_mode(bottom_text, LV_LABEL_LONG_WRAP);
    lv_label_set_text(bottom_text, "Erasing target\n\nDO NOT DISCONNECT\n");

    lv_obj_set_style_text_font(bottom_text, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_color(bottom_text, lv_color_white(), 0);

    lvgl_give_lock();
    return ret;
}

esp_err_t lcd_manager::display_program(uint8_t percentage)
{
    auto ret = clear_display();
    if (ret != ESP_OK) {
        return ret;
    }

    ret = lvgl_take_lock(pdMS_TO_TICKS(1000));

    ret = ret ?: draw_two_bars(&top_bar, &bottom_bar, lv_color_hex(0xffab00), lv_color_white()); // Orange

    auto *top_text = lv_label_create(top_bar);
    lv_obj_align(top_text, LV_ALIGN_CENTER, 0, 0);
    lv_obj_set_style_text_align(top_text, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_text(top_text, "Programming");
    lv_obj_set_style_text_font(top_text, &lv_font_montserrat_28, 0);
    lv_obj_set_style_text_color(top_text, lv_color_white(), 0);

    auto *bottom_progress = lv_bar_create(bottom_bar);
    lv_obj_align(bottom_progress, LV_ALIGN_TOP_LEFT, 0,0);
    lv_obj_set_style_radius(bottom_progress, 0, 0);
    lv_obj_set_style_radius(bottom_progress, 0, LV_PART_INDICATOR);
    lv_obj_set_style_text_align(bottom_progress, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_size(bottom_progress, 240, 120);
    lv_bar_set_value(bottom_progress, percentage, LV_ANIM_OFF);

    lvgl_give_lock();
    return ret;
}

esp_err_t lcd_manager::display_error(const char *err_heading, const char *err_msg)
{
    auto ret = clear_display();
    if (ret != ESP_OK) {
        return ret;
    }

    ret = lvgl_take_lock(pdMS_TO_TICKS(1000));
    ret = ret ?: draw_two_bars(&top_bar, &bottom_bar, lv_color_black(), lv_color_hex(0xff0000)); // Red

    auto *top_text = lv_label_create(top_bar);
    lv_obj_align(top_text, LV_ALIGN_CENTER, 0, -15);
    lv_obj_set_style_text_align(top_text, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_text_fmt(top_text, "Error\n%s", err_heading == nullptr ? "Unknown" : err_heading);
    lv_obj_set_style_text_font(top_text, &lv_font_montserrat_24, 0);
    lv_obj_set_style_text_color(top_text, lv_color_white(), 0);

    auto *bottom_text = lv_label_create(bottom_bar);
    lv_obj_align(bottom_text, LV_ALIGN_TOP_LEFT, 10, 2);
    lv_obj_set_style_text_align(bottom_text, LV_TEXT_ALIGN_LEFT, 0);

    if (err_msg != nullptr) {
        lv_label_set_text(bottom_text, err_msg);
    } else {
        lv_label_set_text(bottom_text, "No error message");
    }

    lv_obj_set_style_text_font(bottom_text, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(bottom_text, lv_color_white(), 0);

    lvgl_give_lock();
    return ret;
}

esp_err_t lcd_manager::display_verify(const char *verify_msg)
{
    auto ret = clear_display();
    if (ret != ESP_OK) {
        return ret;
    }

    ret = lvgl_take_lock(pdMS_TO_TICKS(1000));
    ret = ret ?: draw_two_bars(&top_bar, &bottom_bar, lv_color_hex(0x512da8), lv_color_white()); // Red

    auto *top_text = lv_label_create(top_bar);
    lv_obj_align(top_text, LV_ALIGN_CENTER, 0, 0);
    lv_obj_set_style_text_align(top_text, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_text_fmt(top_text, "Verifying");
    lv_obj_set_style_text_font(top_text, &lv_font_montserrat_28, 0);
    lv_obj_set_style_text_color(top_text, lv_color_white(), 0);

    auto *bottom_text = lv_label_create(bottom_bar);
    lv_obj_align(bottom_text, LV_ALIGN_TOP_LEFT, 10, 6);
    lv_obj_set_style_text_align(bottom_text, LV_TEXT_ALIGN_LEFT, 0);

    if (verify_msg != nullptr) {
        lv_label_set_text(bottom_text, verify_msg);
    } else {
        lv_label_set_text(bottom_text, "Please wait..");
    }

    lv_obj_set_style_text_font(bottom_text, &lv_font_montserrat_18, 0);
    lv_obj_set_style_text_color(bottom_text, lv_color_black(), 0);

    lvgl_give_lock();
    return ret;
}

esp_err_t lcd_manager::display_done()
{
    auto ret = clear_display();
    if (ret != ESP_OK) {
        return ret;
    }

    ret = lvgl_take_lock(pdMS_TO_TICKS(1000));

    if (curr_state != lcd::STATE_ERASE || root_obj == nullptr) {
        ret = ret ?: draw_two_bars(&top_bar, &bottom_bar, lv_color_hex(0x1b5e20), lv_color_white()); // Red
        curr_state = lcd::STATE_DONE;
    } else {
        lvgl_give_lock();
        return ESP_OK;
    }

    auto *top_text = lv_label_create(top_bar);
    lv_obj_align(top_text, LV_ALIGN_CENTER, 0, 0);
    lv_obj_set_style_text_align(top_text, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_text_fmt(top_text, "Done!");
    lv_obj_set_style_text_font(top_text, &lv_font_montserrat_28, 0);
    lv_obj_set_style_text_color(top_text, lv_color_white(), 0);

    auto *bottom_text = lv_label_create(bottom_bar);
    lv_obj_align(bottom_text, LV_ALIGN_CENTER, 0, 6);
    lv_obj_set_style_text_align(bottom_text, LV_ALIGN_CENTER, 0);
    lv_obj_set_size(bottom_text, 200, 100);
    lv_label_set_text(bottom_text, "Please connect to the next product and press the button to continue...");
    lv_obj_set_style_text_font(bottom_text, &lv_font_montserrat_18, 0);
    lv_obj_set_style_text_color(bottom_text, lv_color_black(), 0);


    lvgl_give_lock();
    return ret;
}

esp_err_t lcd_manager::clear_display()
{
    auto ret = lvgl_take_lock(pdMS_TO_TICKS(1000));
    if (ret != ESP_OK) {
        return ret;
    }

    if (root_obj != nullptr) {
        lv_obj_del(root_obj);
    }

    root_obj = lv_obj_create(lv_scr_act());
    lv_obj_set_size(root_obj, 240, 240);
    lv_obj_set_align(root_obj, LV_ALIGN_TOP_LEFT);
    lv_obj_set_pos(root_obj, 0, 0);
    lv_obj_set_style_radius(root_obj, 0, 0);
    lv_obj_set_scrollbar_mode(root_obj, LV_SCROLLBAR_MODE_OFF);
    lv_obj_set_style_pad_all(root_obj, 0, 0);
    lv_obj_set_style_border_width(root_obj, 0, 0);

    lvgl_give_lock();
    return ESP_OK;
}

esp_err_t lcd_manager::draw_two_bars(lv_obj_t **top_out, lv_obj_t **bottom_out, lv_color_t top_color, lv_color_t bottom_color)
{
    if (top_out == nullptr || bottom_out == nullptr) {
        return ESP_ERR_INVALID_ARG;
    }

    if (root_obj == nullptr) {
        return ESP_ERR_INVALID_STATE;
    }

    auto *top_obj = lv_obj_create(root_obj);
    if (top_obj == nullptr) {
        return ESP_FAIL;
    }

    lv_obj_set_size(top_obj, 240, 120);
    lv_obj_set_align(top_obj, LV_ALIGN_TOP_LEFT);
    lv_obj_set_pos(top_obj, 0, 0);
    lv_obj_set_style_radius(top_obj, 0, 0);
    lv_obj_set_style_bg_color(top_obj, top_color, 0);
    lv_obj_set_scrollbar_mode(top_obj, LV_SCROLLBAR_MODE_OFF);
    lv_obj_set_style_pad_all(top_obj, 0, 0);
    lv_obj_set_style_border_width(top_obj, 0, 0);
    *top_out = top_obj;

    auto *bottom_obj = lv_obj_create(root_obj);
    if (bottom_obj == nullptr) {
        return ESP_FAIL;
    }

    lv_obj_set_size(bottom_obj, 240, 120);
    lv_obj_set_align(bottom_obj, LV_ALIGN_TOP_LEFT);
    lv_obj_set_pos(bottom_obj, 0, 120);
    lv_obj_set_style_radius(bottom_obj, 0, 0);
    lv_obj_set_style_bg_color(bottom_obj, bottom_color, 0); // Dark cyan (need white text)
    lv_obj_set_scrollbar_mode(bottom_obj, LV_SCROLLBAR_MODE_OFF);
    lv_obj_set_style_pad_all(bottom_obj, 0, 0);
    lv_obj_set_style_border_width(bottom_obj, 0, 0);
    *bottom_out = bottom_obj;

    return ESP_OK;
}
