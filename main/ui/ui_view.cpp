#include <cstdio>
#include <cstring>

#include "ui_view.hpp"

namespace
{
    constexpr int32_t MARGIN = 12;
    constexpr int32_t HEADER_H = 26;
    constexpr int32_t TITLE_Y = 32;      // Montserrat 32: 35 px lines
    constexpr int32_t BIG_TITLE_Y = 30;  // Montserrat 48: 52 px lines
    constexpr int32_t DETAIL_Y = 72;     // Montserrat 16: 18 px lines, two of them
    constexpr int32_t BIG_DETAIL_Y = 86;
    constexpr int32_t DETAIL_H = 40;
    constexpr int32_t BAR_Y = 120;
    constexpr int32_t BAR_H = 12;
    constexpr int32_t PERCENT_W = 110;

    // Plain, well-separated colours that survive RGB565 and a TFT's viewing
    // angle: text is white on dark colours and black on amber.
    struct palette {
        uint32_t body;
        uint32_t header;
        uint32_t text;
        uint32_t detail;
    };

    constexpr palette NEUTRAL = {0x000000, 0x2a2a2a, 0xffffff, 0xc0c0c0};
    constexpr palette PASS = {0x1b7f35, 0x125c26, 0xffffff, 0xe4f1e7};
    constexpr palette FAIL = {0xb3261e, 0x861c16, 0xffffff, 0xf7e0de};
    constexpr palette NOTICE = {0xe0a300, 0xb58300, 0x000000, 0x1e1800};
    constexpr uint32_t BAR_TRACK = 0x333333;
    constexpr uint32_t BAR_FILL = 0x2f80ed;

    const palette &palette_for(ui_model::tone colour)
    {
        switch (colour) {
        case ui_model::tone::pass:
            return PASS;
        case ui_model::tone::fail:
            return FAIL;
        case ui_model::tone::notice:
            return NOTICE;
        case ui_model::tone::neutral:
            break;
        }
        return NEUTRAL;
    }

    int32_t text_width(const lv_font_t *font, const char *text)
    {
        int32_t width = 0;
        for (const char *c = text; *c != '\0'; c++) {
            width += lv_font_get_glyph_width(font, static_cast<uint8_t>(c[0]), static_cast<uint8_t>(c[1]));
        }
        return width;
    }

    lv_obj_t *make_label(lv_obj_t *parent, const lv_font_t *font, int32_t x, int32_t y, int32_t w, int32_t h)
    {
        lv_obj_t *label = lv_label_create(parent);
        lv_obj_remove_style_all(label);
        lv_obj_set_pos(label, x, y);
        lv_obj_set_size(label, w, h);
        lv_obj_set_style_text_font(label, font, 0);
        lv_label_set_text_static(label, "");
        return label;
    }
} // namespace

void ui_view::create(lv_obj_t *_screen)
{
    screen = _screen;
    lv_obj_clean(screen);
    lv_obj_remove_style_all(screen);
    lv_obj_set_scrollable(screen, false);
    lv_obj_set_style_bg_opa(screen, LV_OPA_COVER, 0);

    header_bar = lv_obj_create(screen);
    lv_obj_remove_style_all(header_bar);
    lv_obj_set_scrollable(header_bar, false);
    lv_obj_set_pos(header_bar, 0, 0);
    lv_obj_set_size(header_bar, WIDTH, HEADER_H);
    lv_obj_set_style_bg_opa(header_bar, LV_OPA_COVER, 0);

    const int32_t header_text_y = (HEADER_H - 18) / 2;
    header_label = make_label(header_bar, &lv_font_montserrat_16, MARGIN, header_text_y, WIDTH / 2, 18);
    link_label = make_label(header_bar, &lv_font_montserrat_16, WIDTH / 2, header_text_y, WIDTH / 2 - MARGIN, 18);
    lv_obj_set_style_text_align(link_label, LV_TEXT_ALIGN_RIGHT, 0);

    title_label = make_label(screen, &lv_font_montserrat_32, MARGIN, TITLE_Y, WIDTH - 2 * MARGIN - PERCENT_W, 52);
    lv_label_set_long_mode(title_label, LV_LABEL_LONG_MODE_CLIP);

    percent_label = make_label(screen, &lv_font_montserrat_32, WIDTH - MARGIN - PERCENT_W, TITLE_Y, PERCENT_W, 35);
    lv_obj_set_style_text_align(percent_label, LV_TEXT_ALIGN_RIGHT, 0);

    detail_label = make_label(screen, &lv_font_montserrat_16, MARGIN, DETAIL_Y, WIDTH - 2 * MARGIN, DETAIL_H);
    lv_label_set_long_mode(detail_label, LV_LABEL_LONG_MODE_DOTS);

    bar = lv_bar_create(screen);
    lv_obj_remove_style_all(bar);
    lv_obj_set_pos(bar, MARGIN, BAR_Y);
    lv_obj_set_size(bar, WIDTH - 2 * MARGIN, BAR_H);
    lv_bar_set_range(bar, 0, 100);
    lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_bg_color(bar, lv_color_hex(BAR_TRACK), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(bar, lv_color_hex(BAR_FILL), LV_PART_INDICATOR);
    lv_obj_set_hidden(bar, true);

    has_shown = false;
}

void ui_view::set_text(lv_obj_t *label, const char *text)
{
    if (strcmp(lv_label_get_text(label), text) != 0) {
        lv_label_set_text(label, text);
    }
}

void ui_view::set_link(const char *text)
{
    if (link_label != nullptr) {
        set_text(link_label, text);
    }
}

void ui_view::apply_colours(ui_model::tone colour)
{
    const palette &p = palette_for(colour);
    lv_obj_set_style_bg_color(screen, lv_color_hex(p.body), 0);
    lv_obj_set_style_bg_color(header_bar, lv_color_hex(p.header), 0);
    lv_obj_set_style_text_color(header_label, lv_color_hex(p.text), 0);
    lv_obj_set_style_text_color(link_label, lv_color_hex(p.text), 0);
    lv_obj_set_style_text_color(title_label, lv_color_hex(p.text), 0);
    lv_obj_set_style_text_color(percent_label, lv_color_hex(p.text), 0);
    lv_obj_set_style_text_color(detail_label, lv_color_hex(p.detail), 0);
}

void ui_view::show(const ui_model &model)
{
    if (screen == nullptr || (has_shown && model == shown)) {
        return;
    }

    if (!has_shown || model.colour != shown.colour) {
        apply_colours(model.colour);
    }

    // A big title that doesn't fit on one line is drawn at the normal size.
    const bool big = model.big_title && text_width(&lv_font_montserrat_48, model.title) <= WIDTH - 2 * MARGIN;
    if (!has_shown || big != shown_big) {
        lv_obj_set_style_text_font(title_label, big ? &lv_font_montserrat_48 : &lv_font_montserrat_32, 0);
        lv_obj_set_y(title_label, big ? BIG_TITLE_Y : TITLE_Y);
        lv_obj_set_y(detail_label, big ? BIG_DETAIL_Y : DETAIL_Y);
        shown_big = big;
    }

    if (!has_shown || (model.progress < 0) != (shown.progress < 0)) {
        // The percentage takes the right of the title row only while there's a bar.
        lv_obj_set_width(title_label, WIDTH - 2 * MARGIN - (model.progress < 0 ? 0 : PERCENT_W));
    }

    set_text(header_label, model.header);
    set_text(title_label, model.title);
    set_text(detail_label, model.detail);

    if (model.progress < 0) {
        lv_obj_set_hidden(bar, true);
        set_text(percent_label, "");
    } else {
        char percent[8] = {};
        snprintf(percent, sizeof(percent), "%d%%", model.progress);
        set_text(percent_label, percent);
        if (lv_bar_get_value(bar) != model.progress) {
            lv_bar_set_value(bar, model.progress, LV_ANIM_OFF);
        }
        lv_obj_set_hidden(bar, false);
    }

    shown = model;
    has_shown = true;
}
