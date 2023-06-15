#pragma once

#include <esp_err.h>
#include <algorithm>

class file_utils
{
private:
    static const constexpr char *TAG = "file_utils";

public:
    static esp_err_t validate_file_crc32(const char *path, uint32_t crc, size_t len = 0)
    {
        uint32_t actual_crc = 0;
        auto ret = get_file_crc32(path, &actual_crc, len);
        if (ret != ESP_OK) {
            return ret;
        }

        if (actual_crc != crc) {
            ESP_LOGE(TAG, "CRC mismatch for firmware %s, expected 0x%lx, actual 0x%lx", path, crc, actual_crc);
            return ESP_ERR_INVALID_CRC;
        } else {
            ESP_LOGI(TAG, "CRC matched for firmware %s, expected 0x%lx, actual 0x%lx", path, crc, actual_crc);
        }

        return ESP_OK;
    }

    static esp_err_t get_file_crc32(const char *path, uint32_t *crc_out, size_t len = 0)
    {
        if (path == nullptr) {
            ESP_LOGE(TAG, "Path is null!");
            return ESP_ERR_INVALID_ARG;
        }

        if (crc_out == nullptr) {
            ESP_LOGE(TAG, "CRC output is null!");
            return ESP_ERR_INVALID_ARG;
        }

        // Checking errno is not thread safe, so we do a fopen to test the existence of this file
        // access() is not an option sometimes, e.g. LittleFS wrapper doesn't implement this function
        FILE *file = fopen(path, "rb");
        if (file == nullptr) {
            ESP_LOGE(TAG, "Failed when opening file: %s", path);
            return ESP_ERR_NOT_FOUND;
        }

        if (len < 1) {
            fseek(file, 0, SEEK_END);
            len = ftell(file);
            fseek(file, 0, SEEK_SET);
        }

        uint32_t actual_crc = 0;
        while(len > 0) {
            uint8_t buf[256] = { 0 };
            auto read_len = std::min(len, sizeof(buf));
            auto actual_read = fread(buf, 1, read_len, file);
            actual_crc = esp_crc32_le(actual_crc, buf, actual_read);
            len -= actual_read;
        }

        *crc_out = actual_crc;
        return ESP_OK;
    }

    static esp_err_t get_len(const char *path, size_t *actual_len)
    {
        if (path == nullptr || actual_len == nullptr) {
            ESP_LOGE(TAG, "Param is null!");
            return ESP_ERR_INVALID_ARG;
        }

        FILE *file = fopen(path, "rb");
        if (file == nullptr) {
            ESP_LOGE(TAG, "Failed when opening file: %s", path);
            return ESP_ERR_NOT_FOUND;
        }

        fseek(file, 0, SEEK_END);
        size_t len = ftell(file);
        if (len < 4 || len % 4 != 0) {
            ESP_LOGE(TAG, "Manifest in a wrong length: %u", len);
            fclose(file);
            return ESP_ERR_INVALID_STATE;
        }

        fclose(file);
        *actual_len = len;
        return ESP_OK;
    }

    static esp_err_t read_to_buf(const char *path, uint8_t *buf_out, size_t buf_len, size_t *actual_len = nullptr)
    {
        if (path == nullptr) {
            ESP_LOGE(TAG, "Path is null!");
            return ESP_ERR_INVALID_ARG;
        }

        if (buf_out == nullptr) {
            ESP_LOGE(TAG, "Buffer is null!");
            return ESP_ERR_INVALID_ARG;
        }

        // Checking errno is not thread safe, so we do a fopen to test the existence of this file
        // access() is not an option sometimes, e.g. LittleFS wrapper doesn't implement this function
        FILE *file = fopen(path, "rb");
        if (file == nullptr) {
            ESP_LOGE(TAG, "Failed when opening file: %s", path);
            return ESP_ERR_NOT_FOUND;
        }

        fseek(file, 0, SEEK_END);
        size_t len = ftell(file);
        if (len < 4 || len % 4 != 0) {
            ESP_LOGE(TAG, "Manifest in a wrong length: %u", len);
            fclose(file);
            return ESP_ERR_INVALID_STATE;
        }

        fseek(file, 0, SEEK_SET);
        if (len > buf_len) {
            ESP_LOGE(TAG, "File too big: buffer %p size %u, expect %u", buf_out, buf_len, len);
            return ESP_ERR_INVALID_SIZE;
        } else {
            if (actual_len != nullptr) {
                *actual_len = len;
            }
        }

        size_t read_pos = 0;
        while(len > 0) {
            uint8_t buf[256] = { 0 };
            auto read_len = std::min(len, sizeof(buf));
            auto actual_read = fread(buf, 1, read_len, file);

            memcpy(buf_out + read_pos, buf, actual_read);
            len -= actual_read;
            read_pos += actual_read;
        }

        fclose(file);
        return ESP_OK;
    }

    static esp_err_t write_to_file(const char *path, const uint8_t *buf, size_t len)
    {
        if (path == nullptr) {
            ESP_LOGE(TAG, "Path is null!");
            return ESP_ERR_INVALID_ARG;
        }

        if (buf == nullptr || len < 1) {
            ESP_LOGE(TAG, "Buffer is null!");
            return ESP_ERR_INVALID_ARG;
        }

        // Checking errno is not thread safe, so we do a fopen to test the existence of this file
        // access() is not an option sometimes, e.g. LittleFS wrapper doesn't implement this function
        FILE *file = fopen(path, "w");
        if (file == nullptr) {
            ESP_LOGE(TAG, "Failed when opening file: %s", path);
            return ESP_ERR_NOT_FOUND;
        }

        if (fwrite(buf, len, 1, file) < 1) {
            ESP_LOGE(TAG, "Write failed for %s from buf %p len %u", path, buf, len);
            return ESP_FAIL;
        }

        fflush(file);
        fclose(file);
        return ESP_OK;
    }
};