#include <ArduinoJson.hpp>
#include <json_file_reader.hpp>
#include <esp_log.h>
#include <esp_mac.h>
#include <esp_flash.h>
#include "config_reader.hpp"

esp_err_t config_reader::load()
{
    auto ret = esp_efuse_mac_get_default(mac_addr);
    ret = ret ?: esp_flash_read_unique_chip_id(esp_flash_default_chip, &flash_sn);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Can't read MAC address or flash SN: 0x%x", ret);
        return ret;
    }

    snprintf((char *)full_sn, sizeof(full_sn), "%02x%02x%02x%02x%02x%02x%16llx",
             mac_addr[0], mac_addr[1], mac_addr[2], mac_addr[3], mac_addr[4], mac_addr[5], flash_sn);

    full_sn[sizeof(full_sn) - 1] = '\0';
    return ret;
}

esp_err_t config_reader::get_wifi_cred(wifi_config_t *cred)
{
    if (cred == nullptr) {
        return ESP_ERR_INVALID_ARG;
    }

    esp_err_t ret = ESP_OK;
    auto nvs_handle = nvs::open_nvs_handle("net", NVS_READONLY, &ret);
    if (!nvs_handle || ret != ESP_OK) {
        ESP_LOGE(TAG, "get_wifi_cred: failed to open NVS: 0x%x", ret);
        return ret;
    }

    ret = nvs_handle->get_string("wifi_ssid", (char *)cred->sta.ssid, sizeof(wifi_sta_config_t::ssid) - 1);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "get_wifi_cred: no SSID found, abort! 0x%x", ret);
        return ret;
    }

    ret = nvs_handle->get_string("wifi_password", (char *)cred->sta.password, sizeof(wifi_sta_config_t::password) - 1);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "get_wifi_cred: no password found 0x%x", ret);
    }

    return ESP_OK;
}

esp_err_t config_reader::get_mode(config_reader::work_mode *mode)
{
    if (mode == nullptr) {
        return ESP_ERR_INVALID_ARG;
    }

    esp_err_t ret = ESP_OK;
    auto nvs_handle = nvs::open_nvs_handle("soulinjector", NVS_READONLY, &ret);
    if (!nvs_handle || ret != ESP_OK) {
        ESP_LOGE(TAG, "get_wifi_cred: failed to open NVS: 0x%x", ret);
        return ret;
    }


    uint32_t mode_val = 0;
    ret = nvs_handle->get_item("work_mode", mode_val);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "get_mode: mode value not found, abort! 0x%x", ret);
        return ret;
    } else {
        *mode = (config_reader::work_mode)mode_val;
    }

    return ESP_OK;
}

uint64_t config_reader::get_flash_sn() const
{
    return flash_sn;
}

esp_err_t config_reader::get_mac_addr(uint8_t *mac_out)
{
    if (mac_out == nullptr) {
        return ESP_ERR_INVALID_ARG;
    }

    memcpy(mac_out, mac_addr, sizeof(mac_addr));
    return ESP_OK;
}

void config_reader::get_full_sn_str(char *sn_out, size_t buf_len)
{
    if (sn_out == nullptr || buf_len == 0) {
        return;
    }

    memcpy(sn_out, full_sn, std::min(buf_len, sizeof(full_sn)));
}

void config_reader::get_full_sn_byte(uint8_t *buf, size_t buf_len)
{
    if (buf == nullptr || buf_len == 0) {
        return;
    }

    memcpy(buf, mac_addr, std::min(buf_len - sizeof(flash_sn), sizeof(mac_addr)));
    memcpy(buf + sizeof(mac_addr), &flash_sn, std::min(buf_len - sizeof(mac_addr), sizeof(flash_sn)));
}

bool config_reader::has_wifi_cred()
{
    return false;
}



