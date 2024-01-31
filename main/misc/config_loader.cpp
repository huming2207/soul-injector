#include <ArduinoJson.hpp>
#include <json_file_reader.hpp>
#include <esp_log.h>
#include "config_loader.hpp"

esp_err_t config_loader::load()
{
    using namespace ArduinoJson;

    auto ret = cfg_reader.load(CFG_FILE);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to load config file: 0x%x, %s", ret, esp_err_to_name(ret));
        return ret;
    }

    auto json_ret = deserializeJson(json_doc, cfg_reader);
    if (json_ret != DeserializationError::Ok) {
        ESP_LOGE(TAG, "Failed to parse config JSON: %d %s", json_ret.code(), json_ret.c_str());
        return ESP_ERR_INVALID_STATE;
    }

    return ESP_OK;
}
