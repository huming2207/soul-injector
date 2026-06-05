#pragma once

#include <ui/ui_if.hpp>

class ui_screen_progress_114 : public ui_screen
{
public:
    ui_screen_progress_114() = default;
    esp_err_t init(lv_obj_t *_base_obj) override;
    void deinit() override;

    void set_progress(uint32_t done, uint32_t full);
    void set_header_text(const char *text);
    void set_comment_text(const char *text);
    void set_progress_bar_color(lv_color_t indicator_color, lv_color_t bg_color);

private:
    lv_obj_t *base_obj = nullptr;
    lv_obj_t *progress_bar = nullptr;
    lv_obj_t *header_label = nullptr;
    lv_obj_t *comment_label = nullptr;
};
