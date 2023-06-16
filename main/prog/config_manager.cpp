#include <cstring>
#include <esp_log.h>
#include <esp_crc.h>
#include <esp_littlefs.h>
#include <ArduinoJson.hpp>

#include "config_manager.hpp"
#include "file_utils.hpp"

char config_manager::manifest_json[131072] = {};

esp_err_t config_manager::init()
{
    esp_err_t ret = ESP_OK;
    if (!esp_littlefs_mounted(STORAGE_PARTITION_LABEL)) {
        esp_vfs_littlefs_conf_t spiffs_config = {};
        spiffs_config.base_path = BASE_PATH;
        spiffs_config.format_if_mount_failed = false;
        spiffs_config.dont_mount = false;
        spiffs_config.partition_label = STORAGE_PARTITION_LABEL;

        ret = esp_vfs_littlefs_register(&spiffs_config);
        if (ret != ESP_OK) {
            ESP_LOGW(TAG, "Storage partition is corrupted or unformatted, formatting now...");
            ret = esp_littlefs_format(STORAGE_PARTITION_LABEL);
            ret = ret ?: esp_vfs_littlefs_register(&spiffs_config);
            if (ret != ESP_OK) {
                ESP_LOGE(TAG, "Failed to initialize LittleFS: %s", esp_err_to_name(ret));
                return ret;
            } else {
                ESP_LOGI(TAG, "Storage filesystem formatted & mounted to %s", BASE_PATH);
            }
        } else {
            ESP_LOGI(TAG, "Storage filesystem mounted to %s", BASE_PATH);
        }
    }

    memset(manifest_json, 0, sizeof(manifest_json));
    ret = ret ?: file_utils::read_to_buf(MANIFEST_PATH, (uint8_t *) (manifest_json), sizeof(manifest_json) - 1, &manifest_json_len);
    if (ret != ESP_OK) {
        return ret;
    }

    auto decode_err = deserializeJson(manifest_doc, manifest_json, manifest_json_len);
    if (decode_err) {
        ESP_LOGE(TAG, "Failed to decode manifest JSON: 0x%x, %s", decode_err.code(), decode_err.c_str());
        return ESP_ERR_INVALID_STATE;
    } else {
        manifest_loaded = true;
    }

    return ret;
}

esp_err_t config_manager::get_algo_name(char *algo_name, size_t len) const
{
    auto name = manifest_doc["algo"]["name"].as<const char *>();
    if (name == nullptr) {
        return ESP_ERR_NOT_FOUND;
    }

    strncpy(algo_name, name, len);
    return ESP_OK;
}

esp_err_t config_manager::get_target_name(char *target_name, size_t len) const
{
    auto name = manifest_doc["target"]["name"].as<const char *>();
    if (name == nullptr) {
        return ESP_ERR_NOT_FOUND;
    }

    strncpy(target_name, name, len);
    return ESP_OK;
}

esp_err_t config_manager::get_algo_bin(uint8_t *algo, size_t len, size_t *actual_len) const
{
    auto name = manifest_doc["algo"]["name"].as<const char *>();
    if (name == nullptr) {
        return ESP_ERR_NOT_FOUND;
    }

    char path_buf[256] = {};
    snprintf(path_buf, sizeof(path_buf), "%s/%s", BASE_PATH, name);
    return file_utils::read_to_buf(path_buf, algo, len, actual_len);
}

esp_err_t config_manager::get_algo_bin_len(size_t *out) const
{
    auto name = manifest_doc["algo"]["name"].as<const char *>();
    if (name == nullptr) {
        return ESP_ERR_NOT_FOUND;
    }

    char path_buf[256] = {};
    snprintf(path_buf, sizeof(path_buf), "%s/%s", BASE_PATH, name);
    return file_utils::get_len(path_buf, out);
}

esp_err_t config_manager::get_ram_size_byte(uint32_t *out) const
{
    if (out == nullptr) {
        return ESP_ERR_INVALID_ARG;
    }

    *out = manifest_doc["target"]["ram_size"].as<uint32_t>();
    return ESP_OK;
}

esp_err_t config_manager::get_flash_size_byte(uint32_t *out) const
{
    if (out == nullptr) {
        return ESP_ERR_INVALID_ARG;
    }

    *out = manifest_doc["target"]["flash_size"].as<uint32_t>();
    return ESP_OK;
}

esp_err_t config_manager::get_pc_init(uint32_t *out) const
{
    if (out == nullptr) {
        return ESP_ERR_INVALID_ARG;
    }

    *out = manifest_doc["algo"]["pc"]["init"].as<uint32_t>();
    return ESP_OK;
}

esp_err_t config_manager::get_pc_uninit(uint32_t *out) const
{
    if (out == nullptr) {
        return ESP_ERR_INVALID_ARG;
    }

    *out = manifest_doc["algo"]["pc"]["uninit"].as<uint32_t>();
    return ESP_OK;
}

esp_err_t config_manager::get_pc_program_page(uint32_t *out) const
{
    if (out == nullptr) {
        return ESP_ERR_INVALID_ARG;
    }

    *out = manifest_doc["algo"]["pc"]["prog_page"].as<uint32_t>();
    return ESP_OK;
}

esp_err_t config_manager::get_pc_erase_sector(uint32_t *out) const
{
    if (out == nullptr) {
        return ESP_ERR_INVALID_ARG;
    }

    *out = manifest_doc["algo"]["pc"]["erase_sector"].as<uint32_t>();
    return ESP_OK;
}

esp_err_t config_manager::get_pc_erase_all(uint32_t *out) const
{
    if (out == nullptr) {
        return ESP_ERR_INVALID_ARG;
    }

    *out = manifest_doc["algo"]["pc"]["erase_all"].as<uint32_t>();
    return ESP_OK;
}

esp_err_t config_manager::get_pc_verify(uint32_t *out) const
{
    if (out == nullptr) {
        return ESP_ERR_INVALID_ARG;
    }

    *out = manifest_doc["algo"]["pc"]["verify"].as<uint32_t>();
    return ESP_OK;
}

esp_err_t config_manager::get_data_section_offset(uint32_t *out) const
{
    if (out == nullptr) {
        return ESP_ERR_INVALID_ARG;
    }

    *out = manifest_doc["algo"]["addr"]["data_section"].as<uint32_t>();
    return ESP_OK;
}

esp_err_t config_manager::get_flash_start_addr(uint32_t *out) const
{
    if (out == nullptr) {
        return ESP_ERR_INVALID_ARG;
    }

    *out = manifest_doc["target"]["nvm"]["start"].as<uint32_t>();
    return ESP_OK;
}

esp_err_t config_manager::get_flash_end_addr(uint32_t *out) const
{
    if (out == nullptr) {
        return ESP_ERR_INVALID_ARG;
    }

    *out = manifest_doc["target"]["nvm"]["end"].as<uint32_t>();
    return ESP_OK;
}

esp_err_t config_manager::get_page_size(uint32_t *out) const
{
    if (out == nullptr) {
        return ESP_ERR_INVALID_ARG;
    }

    *out = manifest_doc["target"]["nvm"]["page_size"].as<uint32_t>();
    return ESP_OK;
}

esp_err_t config_manager::get_erased_byte_val(uint32_t *out) const
{
    if (out == nullptr) {
        return ESP_ERR_INVALID_ARG;
    }

    *out = manifest_doc["target"]["nvm"]["erased_byte"].as<uint32_t>();
    return ESP_OK;
}

esp_err_t config_manager::get_program_page_timeout(uint32_t *out) const
{
    if (out == nullptr) {
        return ESP_ERR_INVALID_ARG;
    }

    *out = manifest_doc["target"]["nvm"]["prog_timeout"].as<uint32_t>();
    return ESP_OK;
}

esp_err_t config_manager::get_erase_sector_timeout(uint32_t *out) const
{
    if (out == nullptr) {
        return ESP_ERR_INVALID_ARG;
    }

    *out = manifest_doc["target"]["nvm"]["erase_timeout"].as<uint32_t>();
    return ESP_OK;
}

esp_err_t config_manager::get_sector_size(uint32_t *out) const
{
    if (out == nullptr) {
        return ESP_ERR_INVALID_ARG;
    }

    *out = manifest_doc["target"]["nvm"]["sector_size"].as<uint32_t>();
    return ESP_OK;
}

esp_err_t config_manager::get_fw_crc(uint32_t *out) const
{
    if (out == nullptr) {
        return ESP_ERR_INVALID_ARG;
    }

    *out = manifest_doc["target"]["csum"].as<uint32_t>();
    return ESP_OK;
}

esp_err_t config_manager::get_algo_crc(uint32_t *out) const
{
    if (out == nullptr) {
        return ESP_ERR_INVALID_ARG;
    }

    *out = manifest_doc["algo"]["csum"].as<uint32_t>();
    return ESP_OK;
}

esp_err_t config_manager::reload_cfg()
{
    memset(manifest_json, 0, sizeof(manifest_json));
    auto ret = file_utils::read_to_buf(MANIFEST_PATH, (uint8_t *) (manifest_json), sizeof(manifest_json) - 1, &manifest_json_len);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to load %s", MANIFEST_PATH);
        return ret;
    }

    manifest_doc.clear();
    auto decode_err = deserializeJson(manifest_doc, manifest_json, manifest_json_len);
    if (decode_err) {
        ESP_LOGE(TAG, "Failed to decode manifest JSON: 0x%x, %s", decode_err.code(), decode_err.c_str());
        return ESP_ERR_INVALID_STATE;
    } else {
        manifest_loaded = true;
    }

    return ret;
}

esp_err_t config_manager::read_cfg(char *out, size_t len) const
{
    if (out == nullptr || len < manifest_json_len) {
        return ESP_ERR_INVALID_ARG;
    }

    return file_utils::read_to_buf(MANIFEST_PATH, (uint8_t *) out, len);
}

bool config_manager::has_valid_cfg() const
{
    return manifest_loaded;
}
