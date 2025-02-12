#pragma once

#include <ui/ui_if.hpp>

class ui_screen_current_114 : public ui_screen
{
public:
    ui_screen_current_114() = default;
    esp_err_t init(lv_obj_t *base_obj) override;
    void deinit() override;

    void set_current(const char *text);
    void set_energy(const char *text);
    void set_state(const char *text, lv_color_t bg_color);

private:
    lv_obj_t *header_label = nullptr;
    lv_obj_t *current_label = nullptr;
    lv_obj_t *energy_label = nullptr;
    lv_obj_t *state_label = nullptr;
};
