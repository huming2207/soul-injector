#pragma once

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/timers.h>
#include <freertos/event_groups.h>
#include <esp_err.h>
#include <esp_pm.h>
#include <soc/gpio_num.h>
#include "wear_levelling.h"
#include "mqtt_client.hpp"
#include "display_manager.hpp"
#include <tinyusb.h>
#include <tinyusb_default_config.h>


class bootstrap_fsm
{
public:
    enum evt_bits : uint32_t {
        BIT_RUN_REQUEST = (1UL << 0UL),
        BIT_DETECTED = (1UL << 1UL),        // the trigger's edge on the detect input
        BIT_TRIGGER_CHANGED = (1UL << 2UL), // a job push set the trigger
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
    bool swd_target_arrived();
    void update_usb_power();

private:
    bool last_det_state = false;
    bool swd_present = false;        // TRIGGER_DETECT_SWD: the target answered and hasn't gone since
    int64_t swd_absent_since_us = 0; // first failed probe while swd_present; 0: none
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

private:
    static const constexpr char TAG[] = "bootstrap_fsm";
    static const constexpr gpio_num_t DET_IO_PIN = static_cast<gpio_num_t>(CONFIG_SI_TARGET_DETECT_PIN);
    static const constexpr gpio_num_t PLUG_DET_PIN = static_cast<gpio_num_t>(CONFIG_SI_USB_PLUG_DET_PIN);
    static const constexpr char DATA_PARTITION_PATH[] = "/data";
    static const constexpr uint32_t TRAP_YIELD_WAIT_MS = 2000;
    static const constexpr uint32_t SWD_POLL_MS = 500;  // TRIGGER_DETECT_SWD probe period
    static const constexpr uint32_t SWD_GONE_MS = 1000; // no answer this long: the target went away
    static const constexpr char DATA_PARTITION_LABEL[] = "data";
};
