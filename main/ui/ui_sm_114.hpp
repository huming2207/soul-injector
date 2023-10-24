#pragma once

#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <lvgl.h>
#include "ui_sm_if.hpp"
#include "ui_producer_sm.hpp"

class ui_consumer_sm_114 : public ui_consumer_sm_if
{
public:
    esp_err_t init() override;
    esp_err_t deinit() override;

public:
    // These functions below should be called in UI thread only
    esp_err_t draw_init(ui_state::queue_item *screen)  override;
    esp_err_t draw_erase(ui_state::queue_item *screen) override;
    esp_err_t draw_flash(ui_state::queue_item *screen) override;
    esp_err_t draw_test(ui_state::queue_item *screen)  override;
    esp_err_t draw_error(ui_state::queue_item *screen) override;

private:
    QueueHandle_t ui_queue = nullptr;
    lv_obj_t *base_obj = nullptr;
    lv_obj_t *top_sect = nullptr;
    lv_obj_t *bottom_sect = nullptr;

private:
    static const constexpr char TAG[] = "ui_114";
};