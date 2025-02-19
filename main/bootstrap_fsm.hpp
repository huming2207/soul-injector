#pragma once

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/timers.h>
#include <freertos/event_groups.h>
#include <esp_err.h>
#include <soc/gpio_num.h>
#include "config_reader.hpp"
#include "wifi_manager.hpp"
#include "mqtt_client.hpp"
#include "cohere_flasher.hpp"
#include "esp_littlefs.h"
#include "display_manager.hpp"

class bootstrap_fsm
{
public:
    enum evt_bits : uint32_t
    {
        BIT_TARGET_DISCONNECTED = (1UL << 0UL),
        BIT_TARGET_CONNECTED = (1UL << 0UL),
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
    static esp_err_t setup_storage();
    static void start_offline_flasher();
    static void fsm_task_handler(void *_ctx);
    static IRAM_ATTR void det_io_isr_handler(void *_ctx);
    static void det_pin_debounce_timer(TimerHandle_t timer_handle);

private:
    void run_fsm_task();

private:
    bool last_det_state = false;
    TaskHandle_t fsm_task = nullptr;
    TimerHandle_t det_debounce_timer = nullptr;
    EventGroupHandle_t evt_group = nullptr;
    display_manager *display = nullptr;
    ui_composer *composer = nullptr;
    wifi_manager wifi = {};

private:
    static const constexpr char TAG[] = "bootstrap_fsm";
    static const constexpr esp_vfs_littlefs_conf_t lfs_cfg = {
            .base_path = "/data",
            .partition_label = "data",
            .partition = nullptr,
            .format_if_mount_failed = true,
            .read_only = false,
            .dont_mount = false,
            .grow_on_mount = false,
    };

    static const constexpr gpio_num_t DET_IO_PIN = GPIO_NUM_5;

};

