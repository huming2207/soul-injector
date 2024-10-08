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

    ret = cfg_reader.load(CFG_FILE);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to load config file: 0x%x, %s", ret, esp_err_to_name(ret));
        has_valid_config = false;
        return ret;
    }

    return reload_config();
}

esp_err_t config_reader::get_wifi_cred(wifi_config_t *cred)
{
    if (cred == nullptr) {
        return ESP_ERR_INVALID_ARG;
    }

    const char *ssid_str = json_doc["net"]["wifi"]["ssid"];
    if (ssid_str != nullptr) {
        strncpy((char *)cred->sta.ssid, ssid_str, sizeof(wifi_sta_config_t::ssid));
    } else {
        ESP_LOGE(TAG, "No SSID found!");
        return ESP_ERR_NOT_FINISHED;
    }

    const char *pwd_str = json_doc["net"]["wifi"]["pwd"];
    if (pwd_str != nullptr) {
        strncpy((char *)cred->sta.password, ssid_str, sizeof(wifi_sta_config_t::password));
    }

    return ESP_OK;
}

esp_err_t config_reader::get_mqtt_cred(config::mqtt_cred &mq_cred)
{
    if (!json_doc["net"]["mqtt"] || !json_doc["net"]["mqtt"]["username"] || !json_doc["net"]["mqtt"]["url"]) {
        ESP_LOGE(TAG, "Lacking MQTT cred fields in config!");
        return ESP_ERR_INVALID_ARG;
    }

    std::string client_id = {};
    if (json_doc["net"]["mqtt"]["client_id"]) {
        client_id = std::string(json_doc["net"]["mqtt"]["client_id"]);
    } else {
        client_id = "soul-";
        client_id += full_sn;
    }

    std::string username = json_doc["net"]["mqtt"]["username"];
    std::string password = json_doc["net"]["mqtt"]["password"];
    std::string url = json_doc["net"]["mqtt"]["url"];

    mq_cred.client_id = client_id;
    mq_cred.password = password;
    mq_cred.username = username;

    return ESP_OK;
}

esp_err_t config_reader::get_mode(config_reader::work_mode *mode)
{
    if (mode == nullptr) {
        return ESP_ERR_INVALID_ARG;
    }

    if (!has_valid_config) {
        *mode = OFFLINE_MODE;
        return ESP_OK;
    }

    if (!json_doc["net"] || !json_doc["net"].is<ArduinoJson::JsonObject>()) {
        ESP_LOGW(TAG, "No valid network field in config");
        *mode = OFFLINE_MODE;
        return ESP_OK;
    }

    if (!json_doc["mode"] || !json_doc["mode"].is<const char *>()) {
        ESP_LOGW(TAG, "No valid mode field in config");
        *mode = OFFLINE_MODE;
        return ESP_OK;
    }

    std::string_view mode_str = json_doc["mode"];
    if (mode_str == "cohere" || mode_str == "soyuz") {
        *mode = COHERE_MODE;
    } else if (mode_str == "offline") {
        *mode = OFFLINE_MODE;
    } else {
        ESP_LOGW(TAG, "Unknown mode: %.*s (set to offline mode anyway)", mode_str.length(), mode_str.data());
        *mode = OFFLINE_MODE;
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

esp_err_t config_reader::reload_config()
{
    auto json_ret = ArduinoJson::deserializeJson(json_doc, cfg_reader);
    if (json_ret != ArduinoJson::DeserializationError::Ok) {
        ESP_LOGE(TAG, "Failed to parse config JSON: %d %s", json_ret.code(), json_ret.c_str());

        if (json_ret == ArduinoJson::DeserializationError::EmptyInput
            || json_ret == ArduinoJson::DeserializationError::IncompleteInput || json_ret == ArduinoJson::DeserializationError::InvalidInput) {
            return ESP_ERR_NOT_SUPPORTED;
        } else if (json_ret == ArduinoJson::DeserializationError::NoMemory || json_ret == ArduinoJson::DeserializationError::TooDeep) {
            return ESP_ERR_NO_MEM;
        }

        return ESP_ERR_INVALID_STATE;
    }

    has_valid_config = true;
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



