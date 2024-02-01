#pragma once

#define ARDUINOJSON_ENABLE_STRING_VIEW 1
#include <ArduinoJson.hpp>

namespace config
{
    struct wifi_cred
    {
        char ssid[32];
        char password[64];
    };
}

class config_loader
{
public:
    static config_loader *instance()
    {
        static config_loader _instance;
        return &_instance;
    }

    void operator=(config_loader const &) = delete;
    config_loader(config_loader const &) = delete;

public:
    esp_err_t load();
    esp_err_t get_wifi_cred(config::wifi_cred *cred);

private:
    config_loader() = default;
    json_file_reader cfg_reader {};
    ArduinoJson::JsonDocument json_doc{};

private:
    static const constexpr char TAG[] = "cfg_ldr";
    static const constexpr char CFG_FILE[] = "/data/config.json";
};
