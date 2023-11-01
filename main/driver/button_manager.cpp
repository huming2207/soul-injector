#include <esp_log.h>
#include <driver/gpio.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <freertos/task.h>
#include <hal/gpio_ll.h>
#include "button_manager.hpp"

esp_err_t button_manager::init()
{
    gpio_config_t button_io_cfg = {};
    button_io_cfg.pull_up_en = GPIO_PULLUP_ENABLE;
    button_io_cfg.pull_down_en = GPIO_PULLDOWN_DISABLE;
    button_io_cfg.pin_bit_mask = (1 << GPIO_NUM_0);
    button_io_cfg.intr_type = GPIO_INTR_ANYEDGE;
    button_io_cfg.mode = GPIO_MODE_INPUT;

    auto ret = gpio_config(&button_io_cfg);
    gpio_install_isr_service(0);
    ret = ret ?: gpio_isr_handler_add(BUTTON_GPIO, btn_isr_handler, this);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Button setup failed: 0x%x", ret);
        return ret;
    }

    btn_evt_queue = xQueueCreate(8, sizeof(int));
    if (btn_evt_queue == nullptr) {
        ESP_LOGE(TAG, "Button event queue error");
    }

    xTaskCreate(btn_task_handler, "btn_task", 3072, this, tskIDLE_PRIORITY + 10, nullptr);
    return ret;
}

void button_manager::btn_isr_handler(void *_ctx)
{
    auto *ctx = (button_manager *)_ctx;
    int level = gpio_ll_get_level(&GPIO, BUTTON_GPIO);
    xQueueSendFromISR(ctx->btn_evt_queue, &level, nullptr);
}

void button_manager::btn_task_handler(void *_ctx)
{
    auto *ctx = (button_manager *)_ctx;
    while (true) {
        int level = 0;
        if (xQueueReceive(ctx->btn_evt_queue, &level, portMAX_DELAY) == pdTRUE) {
            if (level > 0) {
                ctx->release_ts = esp_timer_get_time();
                int64_t time_diff = ctx->release_ts - ctx->press_ts;
                if (time_diff < (ctx->glitch_filter_thresh * 1000LL)) {
                    ESP_LOGD(TAG, "Ignore glitch: %lld", time_diff);
                    continue;
                }

                if (ctx->cb == nullptr) {
                    continue;
                }

                if (time_diff >= (ctx->long_press_thresh * 1000LL)) {
                    ctx->cb->handle_long_press();
                } else {
                    ctx->cb->handle_short_press();
                }
            } else {
                ctx->press_ts = esp_timer_get_time();
            }
        }
    }
}

void button_manager::set_handler(button_handle_cb *handler, uint32_t long_press_thresh_ms, uint32_t glitch_filter_thresh_ms)
{
    cb = handler;
    long_press_thresh = long_press_thresh_ms;
    glitch_filter_thresh = glitch_filter_thresh_ms;
}
