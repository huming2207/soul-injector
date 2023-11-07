#pragma once

#include <esp_err.h>
#include <hal/gpio_types.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>

class button_handle_cb
{
public:
    virtual void handle_long_press() = 0;
    virtual void handle_short_press() = 0;
};

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
    void set_handler(button_handle_cb *handler, uint32_t long_press_thresh_ms = 5000, uint32_t glitch_filter_thresh_ms = 20);

public:
    static IRAM_ATTR void btn_isr_handler(void *_ctx);
    static void btn_task_handler(void *_ctx);

private:
    QueueHandle_t btn_evt_queue = nullptr;
    uint32_t long_press_thresh = 5000;
    uint32_t glitch_filter_thresh = 50;
    int64_t press_ts = 0;
    int64_t release_ts = 0;
    button_handle_cb *cb = nullptr;

private:
    static const constexpr char TAG[] = "btn_mgr";
    static const constexpr gpio_num_t BUTTON_GPIO = GPIO_NUM_0;
};

