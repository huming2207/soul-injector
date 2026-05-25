#pragma once

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/timers.h>
#include <freertos/event_groups.h>
#include <esp_err.h>
#include <soc/gpio_num.h>
#include "config_reader.hpp"
#include "wear_levelling.h"
#include "wifi_manager.hpp"
#include "mqtt_client.hpp"
#include "display_manager.hpp"
#include <tinyusb.h>
#include <tinyusb_default_config.h>
#include <tinyusb_msc.h>
#include <tinyusb_cdc_acm.h>

class bootstrap_fsm
{
public:
    enum evt_bits : uint32_t
    {
        BIT_TARGET_DISCONNECTED = (1UL << 0UL),
        BIT_TARGET_CONNECTED = (1UL << 1UL),
    };

public:
    static bootstrap_fsm *instance()
    {
        static bootstrap_fsm _instance;
        return &_instance;
    }

    bootstrap_fsm(bootstrap_fsm const &) = delete;
    void operator=(bootstrap_fsm const &) = delete;

private:
    bootstrap_fsm() = default;

public:
    esp_err_t init();

private:
    esp_err_t setup_storage();
    static void fsm_task_handler(void *_ctx);
    static IRAM_ATTR void det_io_isr_handler(void *_ctx);
    static void det_pin_debounce_timer(TimerHandle_t timer_handle);

private:
    void run_fsm_task();

private:
    bool last_det_state = false;
    wl_handle_t wl_handle = WL_INVALID_HANDLE;
    tinyusb_msc_storage_handle_t tusb_msc_handle = nullptr;
    TaskHandle_t fsm_task = nullptr;
    TimerHandle_t det_debounce_timer = nullptr;
    EventGroupHandle_t evt_group = nullptr;
    display_manager *display = nullptr;
    ui_composer *composer = nullptr;
    char sn_str[32] = { 0 };
    wifi_manager wifi = {};

private:
    static const constexpr char TAG[] = "bootstrap_fsm";
    static const constexpr gpio_num_t DET_IO_PIN = GPIO_NUM_5;
    static const constexpr char DATA_PARTITION_PATH[] = "/data";

};
