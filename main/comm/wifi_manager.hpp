#pragma once

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

private:
    wifi_manager() = default;

private:
    static const constexpr char TAG[] = "wifi_mgr";
};

