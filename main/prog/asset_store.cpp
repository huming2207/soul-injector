#include "asset_store.hpp"

#include <cstring>
#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>

#include <esp_log.h>
#include <esp_vfs_fat.h>

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
    // FAT matches names without case and drops trailing dots, so only one spelling of each name is accepted
    size_t len = strnlen(file_name, MAX_NAME_LEN + 1);
    if (len == 0 || len > MAX_NAME_LEN || file_name[0] == '.' || file_name[len - 1] == '.') {
        return false;
    }

    for (size_t i = 0; i < len; i++) {
        char c = file_name[i];
        bool allowed = (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '.' || c == '_' || c == '-';
        if (!allowed) {
            return false;
        }
    }

    // Upload files are managed by the store itself.
    return !has_suffix(file_name, PART_SUFFIX);
}

esp_err_t asset_store::hash_file(const char *path, uint8_t *out)
{
    FILE *file_fp = fopen(path, "rb");
    if (file_fp == nullptr) {
        return ESP_ERR_NOT_FOUND;
    }

    psa_hash_operation_t file_hash = PSA_HASH_OPERATION_INIT;
    bool ok = psa_hash_setup(&file_hash, PSA_ALG_SHA_256) == PSA_SUCCESS;
    uint8_t buf[512];
    size_t read_len = 0;
    while (ok && (read_len = fread(buf, 1, sizeof(buf), file_fp)) > 0) {
        ok = psa_hash_update(&file_hash, buf, read_len) == PSA_SUCCESS;
    }
    ok = ok && ferror(file_fp) == 0;
    fclose(file_fp);

    size_t hash_len = 0;
    ok = ok && psa_hash_finish(&file_hash, out, SHA256_LEN, &hash_len) == PSA_SUCCESS;
    if (!ok) {
        psa_hash_abort(&file_hash);
        ESP_LOGE(TAG, "hash_file: cannot read %s", path);
        return ESP_FAIL;
    }
    return ESP_OK;
}

esp_err_t asset_store::install_file(const char *from, const char *to)
{
    unlink(to);
    if (rename(from, to) != 0) {
        ESP_LOGE(TAG, "install_file: cannot rename %s to %s", from, to);
        return ESP_FAIL;
    }
    return ESP_OK;
}

bool asset_store::is_present(const char *path, uint32_t expect_size, const uint8_t *sha256)
{
    struct stat st = {};
    if (stat(path, &st) != 0 || st.st_size != (off_t)expect_size) {
        return false;
    }

    uint8_t actual[SHA256_LEN] = {};
    return hash_file(path, actual) == ESP_OK && memcmp(actual, sha256, SHA256_LEN) == 0;
}

void asset_store::remove_stale_uploads()
{
    DIR *dir = opendir(BASE_PATH);
    if (dir == nullptr) {
        return;
    }

    // Upload state lives in RAM, so after a reset every partial upload is abandoned
    struct dirent *entry = nullptr;
    while ((entry = readdir(dir)) != nullptr) {
        char part_path[MAX_PATH_LEN] = {};
        if (has_suffix(entry->d_name, PART_SUFFIX) && make_path(part_path, DIR_PREFIX, entry->d_name, "")) {
            ESP_LOGW(TAG, "remove_stale_uploads: removing %s", part_path);
            unlink(part_path);
        }
    }
    closedir(dir);
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
        ret = install_file(part_path, path);
    }

    if (ret != ESP_OK) {
        unlink(part_path);
        return ret;
    }

    ESP_LOGI(TAG, "commit: %s stored, %lu bytes", name, size);
    return ESP_OK;
}
