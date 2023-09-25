#pragma once

#include <esp_err.h>
#include <wear_levelling.h>
#include "tusb_cdc_acm.h"

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
    static esp_err_t unmount_and_expose();

private:
    wl_handle_t wl_handle = WL_INVALID_HANDLE;
    const esp_partition_t *data_part = nullptr;

private:
    static const constexpr char TAG[] = "comm_msc";
    static const constexpr char PART_NAME[] = "data";
    static const constexpr char PART_PATH[] = "/data";
    static const constexpr char USB_DESC_MANUFACTURER[] = "Jackson M Hu";
    static const constexpr char USB_DESC_PRODUCT[] = "Soul Injector";
    static const constexpr char USB_DESC_CDC_NAME[] = "Soul Injector Programmer";
    static const tinyusb_cdcacm_itf_t CDC_CHANNEL = TINYUSB_CDC_ACM_0;
};
