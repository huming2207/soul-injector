#pragma once

#include <ArduinoJson.hpp>
#include <psram_json_allocator.hpp>

struct self_test_task
{

};

class self_test_runner
{
public:
    self_test_runner() = default;
    esp_err_t load(const char *path);

private:

};
