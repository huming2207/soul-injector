#pragma once

#include <esp_netif.h>
#include <esp_event.h>
#include <functional>

class wifi_manager
{
public:
    esp_err_t init();
    void set_lost_ip_cb(const std::function<void()> &cb);
    void set_got_ip_cb(const std::function<void(esp_netif_ip_info_t *)> &cb);

private:
    std::function<void()> lost_ip_cb = {};
    std::function<void(esp_netif_ip_info_t *)> got_ip_cb = {};

    static void event_handler(void *_ctx, esp_event_base_t evt_base, int32_t evt_id, void *evt_data);

private:
    static const constexpr char TAG[] = "wifi_mgr";
};
