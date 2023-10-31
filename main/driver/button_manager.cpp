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

    xTaskCreate(btn_task_handler, "btn_task", 3072, this, tskIDLE_PRIORITY + 10, nullptr);
    btn_evt_queue = xQueueCreate(16, sizeof(int));
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
            int64_t ts = esp_timer_get_time();

        }
    }
}
