#pragma once

#include <functional>

class wifi_manager
{
public:
    static wifi_manager *instance()
    {
        static wifi_manager _instance;
        return &_instance;
    }

    void operator=(wifi_manager const &) = delete;
    wifi_manager(wifi_manager const &) = delete;

public:
    esp_err_t init();
    static void event_handler(void *_ctx, esp_event_base_t evt_base, int32_t evt_id, void *evt_data);
    void set_lost_ip_cb(const std::function<void()> &cb);
    void set_got_ip_cb(const std::function<void(esp_netif_ip_info_t *)> &cb);

private:
    wifi_manager() = default;
    std::function<void()> lost_ip_cb = {};
    std::function<void(esp_netif_ip_info_t *)> got_ip_cb = {};

private:
    static const constexpr char TAG[] = "wifi_mgr";
};

