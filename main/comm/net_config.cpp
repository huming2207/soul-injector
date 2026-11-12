#include "net_config.hpp"

#include <cstring>

#include <esp_log.h>
#include <nvs.h>

static void read_str(nvs_handle_t handle, const char *key, char *buf, size_t buf_len)
{
    size_t len = buf_len;
    if (nvs_get_str(handle, key, buf, &len) != ESP_OK) {
        buf[0] = '\0';
    }
}

template <typename bytes_t> static void read_blob(nvs_handle_t handle, const char *key, bytes_t &out)
{
    size_t len = sizeof(out.bytes);
    out.size = nvs_get_blob(handle, key, out.bytes, &len) == ESP_OK ? len : 0;
}

esp_err_t net_config::read(si_manage_NetConfig &cfg)
{
    cfg = si_manage_NetConfig_init_zero;

    nvs_handle_t handle = 0;
    auto ret = nvs_open(NVS_NAMESPACE, NVS_READONLY, &handle);
    if (ret == ESP_ERR_NVS_NOT_FOUND) {
        return ESP_OK; // Never set up
    } else if (ret != ESP_OK) {
        ESP_LOGE(TAG, "read: cannot open NVS: %s", esp_err_to_name(ret));
        return ret;
    }

    uint8_t enabled = 0;
    nvs_get_u8(handle, KEY_ENABLED, &enabled);
    cfg.enabled = enabled != 0;
    read_str(handle, KEY_SSID, cfg.wifi_ssid, sizeof(cfg.wifi_ssid));
    read_str(handle, KEY_PASSWORD, cfg.wifi_password, sizeof(cfg.wifi_password));
    read_str(handle, KEY_SERVER_URL, cfg.server_url, sizeof(cfg.server_url));
    cfg.has_wifi_password = cfg.wifi_password[0] != '\0';
    read_blob(handle, KEY_SALT, cfg.password_salt);
    read_blob(handle, KEY_VERIFIER, cfg.password_verifier);
    cfg.has_password = cfg.password_salt.size != 0 && cfg.password_verifier.size != 0;
    nvs_close(handle);
    return ESP_OK;
}

esp_err_t net_config::write(const si_manage_NetConfig &cfg)
{
    si_manage_NetConfig stored = si_manage_NetConfig_init_zero;
    if (cfg.keep_password) {
        auto ret = read(stored);
        if (ret != ESP_OK) {
            return ret;
        }
    }
    const bool has_password = cfg.keep_password ? stored.has_password : cfg.password_salt.size != 0 && cfg.password_verifier.size != 0;
    if (cfg.enabled && (cfg.wifi_ssid[0] == '\0' || strncmp(cfg.server_url, "ws://", 5) != 0 || !has_password)) {
        return ESP_ERR_INVALID_ARG;
    }

    nvs_handle_t handle = 0;
    auto ret = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &handle);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "write: cannot open NVS: %s", esp_err_to_name(ret));
        return ret;
    }

    ret = nvs_set_u8(handle, KEY_ENABLED, cfg.enabled ? 1 : 0);
    ret = ret ?: nvs_set_str(handle, KEY_SSID, cfg.wifi_ssid);
    ret = ret ?: nvs_set_str(handle, KEY_SERVER_URL, cfg.server_url);
    if (!cfg.keep_wifi_password) {
        ret = ret ?: nvs_set_str(handle, KEY_PASSWORD, cfg.wifi_password);
    }
    if (!cfg.keep_password) {
        ret = ret ?: nvs_set_blob(handle, KEY_SALT, cfg.password_salt.bytes, cfg.password_salt.size);
        ret = ret ?: nvs_set_blob(handle, KEY_VERIFIER, cfg.password_verifier.bytes, cfg.password_verifier.size);
    }
    ret = ret ?: nvs_commit(handle);
    nvs_close(handle);

    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "write: failed: %s", esp_err_to_name(ret));
    }
    return ret;
}
