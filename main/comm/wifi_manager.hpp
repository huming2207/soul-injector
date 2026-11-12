#pragma once

#include <atomic>
#include <esp_netif.h>
#include <esp_event.h>
#include <functional>

class wifi_manager
{
public:
    /** Start station mode and keep joining @p ssid. An empty @p password joins an open network. */
    esp_err_t init(const char *ssid, const char *password);
    void set_lost_ip_cb(const std::function<void()> &cb);
    void set_got_ip_cb(const std::function<void(esp_netif_ip_info_t *)> &cb);

    bool has_ip() const { return got_ip.load(); }
    /** Dotted IPv4 address; empty without one. */
    const char *get_ip() const { return ip_str; }
    /** Signal strength in dBm, 0 when not connected. */
    int get_rssi() const;
    /** Last disconnect reason (wifi_err_reason_t), 0 if none. */
    uint32_t get_last_reason() const { return last_reason.load(); }
    /** Modem power save: on while idle, off while a session needs quick answers. */
    void set_power_save(bool on);

private:
    std::function<void()> lost_ip_cb = {};
    std::function<void(esp_netif_ip_info_t *)> got_ip_cb = {};
    std::atomic<bool> got_ip{false};
    std::atomic<uint32_t> last_reason{0};
    char ip_str[16] = {};

    static void event_handler(void *_ctx, esp_event_base_t evt_base, int32_t evt_id, void *evt_data);

private:
    static const constexpr char TAG[] = "wifi_mgr";
};
