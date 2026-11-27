#pragma once

#include <lvgl.h>
#include "ui_model.hpp"

// One layout for every screen on the 428 x 142 panel:
//
//   +--------------------------------------------------+
//   | HEADER                              USB connected |  activity and link
//   | Title                                       62 % |
//   | Detail, up to two lines                           |
//   | [==============                                 ] |  only with progress
//   +--------------------------------------------------+
//
// The objects are made once. show() only touches what changed, so an
// unchanged screen costs no drawing and no SPI traffic. No animations.
// Call with the LVGL lock held.
class ui_view
{
public:
    void create(lv_obj_t *screen);
    void show(const ui_model &model);
    void set_link(const char *text);

    static constexpr int32_t WIDTH = 428;
    static constexpr int32_t HEIGHT = 142;

private:
    void apply_colours(ui_model::tone colour);
    static void set_text(lv_obj_t *label, const char *text);

    lv_obj_t *screen = nullptr;
    lv_obj_t *header_bar = nullptr;
    lv_obj_t *header_label = nullptr;
    lv_obj_t *link_label = nullptr;
    lv_obj_t *title_label = nullptr;
    lv_obj_t *percent_label = nullptr;
    lv_obj_t *detail_label = nullptr;
    lv_obj_t *bar = nullptr;

    ui_model shown = {};
    bool has_shown = false;
    bool shown_big = false;
};
