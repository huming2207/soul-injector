#pragma once

#include <freertos/FreeRTOS.h>
#include <freertos/event_groups.h>
#include <esp_err.h>
#include <wear_levelling.h>
#include "tusb_msc_storage.h"

namespace comm
{
    enum msc_events : uint32_t {
        MSC_USB_PLUGGED_IN = BIT(1),
        MSC_MOUNTED = BIT(2),
    };
}

class comm_msc
{
public:
    static comm_msc *instance()
    {
        static comm_msc _instance;
        return &_instance;
    }

    comm_msc(comm_msc const &) = delete;
    void operator=(comm_msc const &) = delete;

private:
    comm_msc() = default;

public:
    esp_err_t init();
    esp_err_t unmount_and_start_msc();
    esp_err_t mount_and_stop_msc();

private:
    tinyusb_msc_spiflash_config_t spiflash_cfg = {};
    const esp_partition_t *data_part = nullptr;
    EventGroupHandle_t msc_evt_group = nullptr;

private:
    static const constexpr char TAG[] = "comm_msc";
    static const constexpr char PART_NAME[] = "data";
    static const constexpr char PART_PATH[] = "/data";
    static const constexpr char USB_DESC_MANUFACTURER[] = "Jackson M Hu";
    static const constexpr char USB_DESC_PRODUCT[] = "Soul Injector";
    static const constexpr char USB_DESC_CDC_NAME[] = "Soul Injector Programmer";
};
