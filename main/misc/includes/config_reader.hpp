#pragma once

#define ARDUINOJSON_ENABLE_STRING_VIEW 1
#include <ArduinoJson.hpp>
#include <esp_wifi_types.h>
#include "psram_json_allocator.hpp"
#include <json_file_reader.hpp>
#include <memory>
#include <nvs_handle.hpp>

namespace config
{
    struct wifi_cred
    {
        char ssid[32];
        char password[64];
    };

    struct mqtt_cred
    {
        std::string url;
        std::string username;
        std::string password;
        std::string client_id;
    };
}

class config_reader
{
public:
    enum work_mode : uint32_t {
        OFFLINE_MODE = 0, // Default mode if network info doesn't exist
        COHERE_MODE = 1,
    };

public:
    static config_reader *instance()
    {
        static config_reader _instance;
        return &_instance;
    }

    void operator=(config_reader const &) = delete;
    config_reader(config_reader const &) = delete;

public:
    esp_err_t load();
    esp_err_t get_mode(work_mode *mode);
    esp_err_t get_wifi_cred(wifi_config_t *cred);
    esp_err_t get_mac_addr(uint8_t *mac_addr);
    void get_full_sn_str(char *sn_out, size_t buf_len);
    void get_full_sn_byte(uint8_t *buf, size_t buf_len);
    [[nodiscard]] uint64_t get_flash_sn() const;
    bool has_wifi_cred();

private:
    config_reader() = default;
    bool has_valid_config = false;
    uint8_t mac_addr[6] = {};
    uint64_t flash_sn = 0;
    char full_sn[32] = {};

private:
    static const constexpr char TAG[] = "cfg_ldr";
    static const constexpr char CFG_FILE[] = "/data/config.json";
};
