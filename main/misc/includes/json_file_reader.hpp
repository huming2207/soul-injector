#pragma once

#include <cstdio>
#include <esp_err.h>

#include <ArduinoJson.hpp>

class json_file_reader
{
public:
    esp_err_t load(const char *path)
    {
        fp = fopen(path, "rb");
        if (fp == nullptr) {
            return ESP_ERR_NOT_FOUND;
        }

        return ESP_OK;
    }

    int read()
    {
        uint8_t buf = 0;
        if (fread(&buf, 1, 1, fp) < 1) {
            return -1;
        } else {
            return buf;
        }
    }

    size_t readBytes(char* buffer, size_t length)
    {
        if (buffer == nullptr || length < 1) {
            return 0;
        }

        return fread(buffer, 1, length, fp);
    }

    ~json_file_reader()
    {
        if (fp == nullptr) {
            return;
        }

        fclose(fp);
    }

private:
    FILE *fp = nullptr;
};