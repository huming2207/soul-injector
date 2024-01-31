#pragma once

#include <ArduinoJson.hpp>

class config_loader
{
public:
    esp_err_t load();

private:
    json_file_reader cfg_reader {};
    ArduinoJson::JsonDocument json_doc{};

private:
    static const constexpr char TAG[] = "cfg_ldr";
    static const constexpr char CFG_FILE[] = "/data/config.json";
};
