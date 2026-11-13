#include <cstdio>
#include <cstring>
#include <esp_wifi.h>
#include <esp_log.h>
#include "wifi_manager.hpp"

esp_err_t wifi_manager::init(const char *ssid, const char *password)
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

    // The SSID and password fields need no terminator when full
    wifi_config_t wifi_cfg = {};
    memcpy(wifi_cfg.sta.ssid, ssid, strnlen(ssid, sizeof(wifi_cfg.sta.ssid)));
    memcpy(wifi_cfg.sta.password, password, strnlen(password, sizeof(wifi_cfg.sta.password)));
    wifi_cfg.sta.threshold.authmode = password[0] == '\0' ? WIFI_AUTH_OPEN : WIFI_AUTH_WPA_PSK;
    wifi_cfg.sta.sae_pwe_h2e = WPA3_SAE_PWE_BOTH; // Zero is "unspecified": WPA3 APs that need H2E refuse it

    ret = esp_wifi_set_mode(WIFI_MODE_STA);
    ret = ret ?: esp_wifi_set_config(WIFI_IF_STA, &wifi_cfg);
    ret = ret ?: esp_wifi_start();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "WiFi start failed: 0x%x %s", ret, esp_err_to_name(ret));
        return ret;
    }

    ESP_LOGI(TAG, "WiFi started, joining '%s'", ssid);
    return ret;
}

int wifi_manager::get_rssi() const
{
    int rssi = 0;
    if (!has_ip() || esp_wifi_sta_get_rssi(&rssi) != ESP_OK) {
        return 0;
    }
    return rssi;
}

void wifi_manager::set_power_save(bool on)
{
    esp_wifi_set_ps(on ? WIFI_PS_MIN_MODEM : WIFI_PS_NONE);
}

void wifi_manager::event_handler(void *_ctx, esp_event_base_t evt_base, int32_t evt_id, void *evt_data)
{
    auto *ctx = (wifi_manager *)_ctx;
    if (evt_base == WIFI_EVENT && evt_id == WIFI_EVENT_STA_START) {
        ESP_LOGI(TAG, "Started, connecting...");
        esp_wifi_connect();
    } else if (evt_base == WIFI_EVENT && evt_id == WIFI_EVENT_STA_DISCONNECTED) {
        auto *event = (wifi_event_sta_disconnected_t *)evt_data;
        ctx->last_reason.store(event->reason);
        ctx->got_ip.store(false);
        ESP_LOGI(TAG, "Disconnected (reason %u), retrying...", event->reason);
        esp_wifi_connect();
    } else if (evt_base == IP_EVENT && evt_id == IP_EVENT_STA_LOST_IP) {
        ESP_LOGW(TAG, "Lost IP!");
        ctx->got_ip.store(false);
        if (ctx->lost_ip_cb) {
            ctx->lost_ip_cb();
        }
    } else if (evt_base == IP_EVENT && evt_id == IP_EVENT_STA_GOT_IP) {
        auto *event = (ip_event_got_ip_t *)evt_data;
        snprintf(ctx->ip_str, sizeof(ctx->ip_str), IPSTR, IP2STR(&event->ip_info.ip));
        ctx->got_ip.store(true);
        ESP_LOGI(TAG, "Got IP %s", ctx->ip_str);
        if (ctx->got_ip_cb) {
            ctx->got_ip_cb(&event->ip_info);
        }
    }
}


void wifi_manager::set_got_ip_cb(const std::function<void(esp_netif_ip_info_t *)> &cb)
{
    got_ip_cb = cb;
}

void wifi_manager::set_lost_ip_cb(const std::function<void()> &cb)
{
    lost_ip_cb = cb;
}
