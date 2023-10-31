#pragma once

#include <esp_err.h>
#include <hal/gpio_types.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>

class button_manager
{
public:
    static button_manager *instance()
    {
        static button_manager _instance;
        return &_instance;
    }

    void operator=(button_manager const &) = delete;
    button_manager(button_manager const &) = delete;

private:
    button_manager() = default;

public:
    esp_err_t init();

public:
    static IRAM_ATTR void btn_isr_handler(void *_ctx);
    static void btn_task_handler(void *_ctx);

private:
    QueueHandle_t btn_evt_queue = nullptr;
    int64_t prev_press_ts = 0;

private:
    static const constexpr char TAG[] = "btn_mgr";
    static const constexpr gpio_num_t BUTTON_GPIO = GPIO_NUM_0;
};

