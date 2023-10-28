#pragma once

#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <lvgl.h>
#include "ui_if.hpp"
#include "ui_commander.hpp"

class ui_composer_114 : public ui_composer_sm
{
public:
    friend class display_manager;
    esp_err_t init() override;

private:
    // These functions below should be called in UI thread only
    esp_err_t draw_init(ui_state::queue_item *screen)  override;
    esp_err_t draw_erase(ui_state::queue_item *screen) override;
    esp_err_t draw_flash(ui_state::queue_item *screen) override;
    esp_err_t draw_test(ui_state::queue_item *screen)  override;
    esp_err_t draw_error(ui_state::queue_item *screen) override;
    esp_err_t draw_done(ui_state::queue_item *screen) override;
    esp_err_t draw_usb(ui_state::queue_item *screen) override;
    esp_err_t wait_and_draw();

private:
    esp_err_t recreate_widget(bool with_comment = false);

private:
    ui_state::display_state curr_state = ui_state::STATE_EMPTY;
    QueueHandle_t ui_queue = nullptr;
    lv_obj_t *disp_obj = nullptr;
    lv_obj_t *base_obj = nullptr;
    lv_obj_t *top_sect = nullptr;
    lv_obj_t *bottom_sect = nullptr;
    lv_obj_t *top_label = nullptr;
    lv_obj_t *bottom_label = nullptr;
    lv_obj_t *bottom_comment = nullptr;

private:
    static const constexpr char TAG[] = "ui_114";
};