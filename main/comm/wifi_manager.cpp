#include <esp_wifi.h>
#include <esp_log.h>
#include "wifi_manager.hpp"
#include "config_loader.hpp"

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
    wifi_cfg.sta.threshold.authmode = WIFI_AUTH_WPA_PSK;
    auto *cfg_manager = config_loader::instance();
    ret = cfg_manager->get_wifi_cred(&wifi_cfg);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "No SSID provided?");
        return ret;
    }

    ret = esp_wifi_set_mode(WIFI_MODE_STA);
    ret = ret ?: esp_wifi_set_config(WIFI_IF_STA, &wifi_cfg);
    ret = ret ?: esp_wifi_start();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "WiFi start failed: 0x%x %s", ret, esp_err_to_name(ret));
        return ret;
    }

    ESP_LOGE(TAG, "WiFi started");

    return ret;
}

void wifi_manager::event_handler(void *_ctx, esp_event_base_t evt_base, int32_t evt_id, void *evt_data)
{
    auto *ctx = (wifi_manager *)_ctx;
    if (evt_base == WIFI_EVENT && evt_id == WIFI_EVENT_STA_START) {
        ESP_LOGI(TAG, "Started, connecting...");
        esp_wifi_connect();
    } else if (evt_base == WIFI_EVENT && evt_id == WIFI_EVENT_STA_DISCONNECTED) {
        ESP_LOGI(TAG, "Disconnected, retrying...");
        esp_wifi_connect();
    } else if (evt_base == IP_EVENT && evt_id == IP_EVENT_STA_LOST_IP) {
        ESP_LOGW(TAG, "Lost IP!");
        if (ctx->lost_ip_cb) {
            ctx->lost_ip_cb();
        }
    } else if (evt_base == IP_EVENT && evt_id == IP_EVENT_STA_GOT_IP) {
        ESP_LOGW(TAG, "Got IP!");
        if (ctx->got_ip_cb) {
            auto *event = (ip_event_got_ip_t*) evt_data;
            ctx->got_ip_cb(&event->ip_info);
        }
    }
}

void wifi_manager::set_lost_ip_cb(const std::function<void()> &cb)
{
    lost_ip_cb = cb;
}

void wifi_manager::set_got_ip_cb(const std::function<void(esp_netif_ip_info_t *)> &cb)
{
    got_ip_cb = cb;
}
