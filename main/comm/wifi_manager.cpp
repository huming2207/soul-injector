#include <esp_wifi.h>
#include <esp_log.h>
#include "wifi_manager.hpp"

esp_err_t wifi_manager::init()
{
    // Ignore errors for these three calls, as they may have been init'ed earlier
    esp_netif_init();
    esp_event_loop_create_default();
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t init_cfg = WIFI_INIT_CONFIG_DEFAULT();
    auto ret = esp_wifi_init(&init_cfg);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "WiFi init failed: 0x%x %s", ret, esp_err_to_name(ret));
        return ret;
    }

    ret = ret ?: esp_event_handler_register(IP_EVENT, ESP_EVENT_ANY_ID, &event_handler, this);
    ret = ret ?: esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &event_handler, this);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "WiFi event attach failed: 0x%x %s", ret, esp_err_to_name(ret));
        return ret;
    }

    wifi_config_t wifi_cfg = {};


    return ret;
}

void wifi_manager::event_handler(void *_ctx, esp_event_base_t evt_base, int32_t evt_id, void *evt_data)
{
    auto *ctx = (wifi_manager *)_ctx;
}
