#pragma once

#include <esp_err.h>

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

private:


private:
    static const constexpr char USB_DESC_MANUFACTURER[] = "Jackson M Hu";
    static const constexpr char USB_DESC_PRODUCT[] = "Soul Injector";
    static const constexpr char USB_DESC_CDC_NAME[] = "Soul Injector Programmer";
};
