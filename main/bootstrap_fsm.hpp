#pragma once

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/timers.h>
#include <freertos/event_groups.h>
#include <esp_err.h>
#include <esp_pm.h>
#include <soc/gpio_num.h>
#include "wear_levelling.h"
#include "wifi_manager.hpp"
#include "mqtt_client.hpp"
#include "display_manager.hpp"
#include <tinyusb.h>
#include <tinyusb_default_config.h>


class bootstrap_fsm
{
public:
    enum evt_bits : uint32_t {
        BIT_RUN_REQUEST = (1UL << 0UL),
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
    esp_err_t setup_pm_locks();
    esp_err_t setup_io_pins();
    esp_err_t setup_storage();
    esp_err_t setup_usb();
    static void fsm_task_handler(void *_ctx);
    static void io_isr_handler(void *_ctx);
    static void io_debounce_handler(TimerHandle_t timer_handle);
    static void arm_io_pin(gpio_num_t pin, int level);

private:
    void run_fsm_task();
    void run_job();
    void show_auto_run_refused();
    void update_target_detect();
    void update_usb_power();

private:
    bool last_det_state = false;
    bool usb_powered = false;
    esp_pm_lock_handle_t run_pm_lock = nullptr; // Held during a run: SWD and the target UART need full speed and no light sleep
    esp_pm_lock_handle_t usb_pm_lock = nullptr; // Held on USB power: USB stops working in light sleep
    wl_handle_t wl_handle = WL_INVALID_HANDLE;
    TaskHandle_t fsm_task = nullptr;
    TimerHandle_t io_debounce_timer = nullptr;
    EventGroupHandle_t evt_group = nullptr;
    display_manager *display = nullptr;
    ui_composer *composer = nullptr;
    char sn_str[32] = {0};
    wifi_manager wifi = {};

private:
    static const constexpr char TAG[] = "bootstrap_fsm";
    static const constexpr gpio_num_t DET_IO_PIN = static_cast<gpio_num_t>(CONFIG_SI_TARGET_DETECT_PIN);
    static const constexpr gpio_num_t PLUG_DET_PIN = static_cast<gpio_num_t>(CONFIG_SI_USB_PLUG_DET_PIN);
    static const constexpr char DATA_PARTITION_PATH[] = "/data";
    static const constexpr char DATA_PARTITION_LABEL[] = "data";
};
