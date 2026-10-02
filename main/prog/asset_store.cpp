#include "asset_store.hpp"

#include <cstring>
#include <sys/stat.h>
#include <unistd.h>

#include <esp_log.h>
#include <esp_vfs_fat.h>

static int hex_char_to_val(char c)
{
    if (c >= '0' && c <= '9')
        return c - '0';
    if (c >= 'a' && c <= 'f')
        return c - 'a' + 10;
    if (c >= 'A' && c <= 'F')
        return c - 'A' + 10;
    return -1;
}

static bool parse_sha256_hex(const char *hex_str, uint8_t *out_bytes)
{
    for (size_t i = 0; i < asset_store::SHA256_LEN; ++i) {
        int high = hex_char_to_val(hex_str[2 * i]);
        int low = hex_char_to_val(hex_str[2 * i + 1]);
        if (high < 0 || low < 0) {
            return false;
        }
        out_bytes[i] = (high << 4) | low;
    }
    return true;
}

static bool has_suffix(const char *str, const char *suffix)
{
    size_t str_len = strlen(str);
    size_t suffix_len = strlen(suffix);
    return str_len >= suffix_len && strcmp(str + str_len - suffix_len, suffix) == 0;
}

bool asset_store::make_path(char *out, const char *prefix, const char *file_name, const char *suffix)
{
    int len = snprintf(out, MAX_PATH_LEN, "%s%s%s", prefix, file_name, suffix);
    return len > 0 && len < (int)MAX_PATH_LEN;
}

bool asset_store::is_valid_name(const char *file_name)
{
    size_t len = strnlen(file_name, MAX_NAME_LEN + 1);
    if (len == 0 || len > MAX_NAME_LEN || file_name[0] == '.') {
        return false;
    }

    for (size_t i = 0; i < len; i++) {
        char c = file_name[i];
        bool allowed = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '.' || c == '_' || c == '-';
        if (!allowed) {
            return false;
        }
    }

    // Upload and hash files are managed by the store itself.
    return !has_suffix(file_name, PART_SUFFIX) && !has_suffix(file_name, SIDECAR_SUFFIX);
}

esp_err_t asset_store::read_sha256(const char *path, uint8_t *out)
{
    char sidecar[MAX_PATH_LEN] = {};
    if (!make_path(sidecar, path, SIDECAR_SUFFIX, "")) {
        return ESP_ERR_INVALID_ARG;
    }

    FILE *sidecar_fp = fopen(sidecar, "r");
    if (sidecar_fp == nullptr) {
        return ESP_ERR_NOT_FOUND;
    }

    char hex[SHA256_LEN * 2] = {};
    size_t read_len = fread(hex, 1, sizeof(hex), sidecar_fp);
    fclose(sidecar_fp);
    if (read_len != sizeof(hex) || !parse_sha256_hex(hex, out)) {
        ESP_LOGE(TAG, "read_sha256: invalid sidecar %s", sidecar);
        return ESP_ERR_INVALID_SIZE;
    }

    return ESP_OK;
}

esp_err_t asset_store::write_sidecar(const char *path, const uint8_t *sha256)
{
    char sidecar[MAX_PATH_LEN] = {};
    if (!make_path(sidecar, path, SIDECAR_SUFFIX, "")) {
        return ESP_ERR_INVALID_ARG;
    }

    FILE *sidecar_fp = fopen(sidecar, "w");
    if (sidecar_fp == nullptr) {
        ESP_LOGE(TAG, "write_sidecar: cannot create %s", sidecar);
        return ESP_FAIL;
    }

    int ret = 0;
    for (size_t i = 0; i < SHA256_LEN && ret >= 0; i++) {
        ret = fprintf(sidecar_fp, "%02x", sha256[i]);
    }
    ret = ret < 0 ? ret : fprintf(sidecar_fp, "\n");
    if (fclose(sidecar_fp) != 0 || ret < 0) {
        ESP_LOGE(TAG, "write_sidecar: cannot write %s", sidecar);
        return ESP_FAIL;
    }
    return ESP_OK;
}

esp_err_t asset_store::install_file(const char *from, const char *to, const uint8_t *sha256)
{
    char to_sidecar[MAX_PATH_LEN] = {};
    if (!make_path(to_sidecar, to, SIDECAR_SUFFIX, "")) {
        return ESP_ERR_INVALID_ARG;
    }

    // Drop the old hash first: a power cut part-way then leaves a file
    // without a sidecar, which counts as absent rather than as the old asset.
    unlink(to_sidecar);
    unlink(to);
    if (rename(from, to) != 0) {
        ESP_LOGE(TAG, "install_file: cannot rename %s to %s", from, to);
        return ESP_FAIL;
    }
    return write_sidecar(to, sha256);
}

bool asset_store::is_present(const char *path, uint32_t expect_size, const uint8_t *sha256)
{
    struct stat st = {};
    if (stat(path, &st) != 0 || st.st_size != (off_t)expect_size) {
        return false;
    }

    uint8_t stored[SHA256_LEN] = {};
    return read_sha256(path, stored) == ESP_OK && memcmp(stored, sha256, SHA256_LEN) == 0;
}

bool asset_store::is_resumable(const char *new_name, uint32_t new_size, const uint8_t *sha256) const
{
    return fp != nullptr && strcmp(name, new_name) == 0 && size == new_size && memcmp(expected_sha256, sha256, SHA256_LEN) == 0;
}

void asset_store::abort_upload()
{
    if (fp == nullptr) {
        return;
    }

    fclose(fp);
    fp = nullptr;
    psa_hash_abort(&hash);

    char part_path[MAX_PATH_LEN] = {};
    if (make_path(part_path, DIR_PREFIX, name, PART_SUFFIX)) {
        unlink(part_path);
    }
    ESP_LOGW(TAG, "abort_upload: dropped %s after %lu of %lu bytes", name, written, size);
}

esp_err_t asset_store::begin(const char *new_name, uint32_t new_size, const uint8_t *sha256, bool *present_out, uint32_t *offset_out)
{
    if (!is_valid_name(new_name) || new_size == 0 || sha256 == nullptr || present_out == nullptr || offset_out == nullptr) {
        return ESP_ERR_INVALID_ARG;
    }
    if (strcmp(new_name, JOB_NAME) == 0) {
        ESP_LOGE(TAG, "begin: %s is replaced through JOB_SET only", JOB_NAME);
        return ESP_ERR_NOT_ALLOWED;
    }

    *present_out = false;
    if (is_resumable(new_name, new_size, sha256)) {
        ESP_LOGI(TAG, "begin: resuming %s at %lu", name, written);
        *offset_out = written;
        return ESP_OK;
    }
    abort_upload();

    char path[MAX_PATH_LEN] = {};
    char part_path[MAX_PATH_LEN] = {};
    if (!make_path(path, DIR_PREFIX, new_name, "") || !make_path(part_path, DIR_PREFIX, new_name, PART_SUFFIX)) {
        return ESP_ERR_INVALID_ARG;
    }
    if (is_present(path, new_size, sha256)) {
        ESP_LOGI(TAG, "begin: %s already present", new_name);
        *present_out = true;
        *offset_out = new_size;
        return ESP_OK;
    }

    uint64_t total_bytes = 0, free_bytes = 0;
    esp_err_t ret = esp_vfs_fat_info(BASE_PATH, &total_bytes, &free_bytes);
    if (ret != ESP_OK || free_bytes < new_size) {
        ESP_LOGE(TAG, "begin: %s needs %lu bytes, %llu free", new_name, new_size, free_bytes);
        return ESP_ERR_NO_MEM;
    }

    hash = psa_hash_operation_init();
    if (psa_hash_setup(&hash, PSA_ALG_SHA_256) != PSA_SUCCESS) {
        return ESP_FAIL;
    }
    fp = fopen(part_path, "wb");
    if (fp == nullptr) {
        ESP_LOGE(TAG, "begin: cannot create %s", part_path);
        psa_hash_abort(&hash);
        return ESP_FAIL;
    }

    strlcpy(name, new_name, sizeof(name));
    memcpy(expected_sha256, sha256, SHA256_LEN);
    size = new_size;
    written = 0;
    *offset_out = 0;
    ESP_LOGI(TAG, "begin: receiving %s, %lu bytes", name, size);
    return ESP_OK;
}

esp_err_t asset_store::write(uint32_t offset, const uint8_t *data, size_t len)
{
    if (fp == nullptr) {
        return ESP_ERR_INVALID_STATE;
    }
    if (offset != written) {
        ESP_LOGE(TAG, "write: offset %lu, expected %lu", offset, written);
        return ESP_ERR_INVALID_ARG;
    }
    if (len == 0 || len > size - written) {
        return ESP_ERR_INVALID_SIZE;
    }

    if (fwrite(data, 1, len, fp) != len || psa_hash_update(&hash, data, len) != PSA_SUCCESS) {
        ESP_LOGE(TAG, "write: failed at %lu", written);
        abort_upload();
        return ESP_FAIL;
    }

    written += len;
    return ESP_OK;
}

esp_err_t asset_store::commit()
{
    if (fp == nullptr) {
        return ESP_ERR_INVALID_STATE;
    }
    if (written != size) {
        ESP_LOGE(TAG, "commit: %s has %lu of %lu bytes", name, written, size);
        return ESP_ERR_INVALID_SIZE;
    }

    uint8_t actual[SHA256_LEN] = {};
    size_t actual_len = 0;
    bool hashed = psa_hash_finish(&hash, actual, sizeof(actual), &actual_len) == PSA_SUCCESS;
    bool closed = fclose(fp) == 0;
    fp = nullptr;

    char path[MAX_PATH_LEN] = {};
    char part_path[MAX_PATH_LEN] = {};
    make_path(path, DIR_PREFIX, name, "");
    make_path(part_path, DIR_PREFIX, name, PART_SUFFIX);

    esp_err_t ret = ESP_OK;
    if (!hashed || !closed) {
        ret = ESP_FAIL;
    } else if (memcmp(actual, expected_sha256, SHA256_LEN) != 0) {
        ESP_LOGE(TAG, "commit: SHA256 mismatch for %s", name);
        ret = ESP_ERR_INVALID_CRC;
    } else {
        ret = install_file(part_path, path, actual);
    }

    if (ret != ESP_OK) {
        unlink(part_path);
        return ret;
    }

    ESP_LOGI(TAG, "commit: %s stored, %lu bytes", name, size);
    return ESP_OK;
}
