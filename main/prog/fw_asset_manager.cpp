#include "fw_asset_manager.hpp"

#include <cctype>
#include <cerrno>
#include <cstdio>
#include <cstring>

#include <esp_heap_caps.h>
#include <esp_log.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <psa/crypto.h>

#include "config/job_decoder.hpp"

// -------------------------------------------------------------------
// SHA256 helpers
// -------------------------------------------------------------------

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
    for (int i = 0; i < 32; ++i) {
        int high = hex_char_to_val(hex_str[2 * i]);
        int low = hex_char_to_val(hex_str[2 * i + 1]);
        if (high < 0 || low < 0) {
            return false;
        }
        out_bytes[i] = (high << 4) | low;
    }
    return true;
}

esp_err_t fw_asset_manager::get_sha256_from_file(const char *path, uint8_t *out)
{
    if (path == nullptr || out == nullptr) {
        return ESP_ERR_INVALID_ARG;
    }

    // Check if the path ends with ".sha256"
    size_t path_len = strlen(path);
    const char *suffix = ".sha256";
    size_t suffix_len = strlen(suffix);
    bool is_sha256_file = path_len >= suffix_len && strcmp(path + path_len - suffix_len, suffix) == 0;

    if (is_sha256_file) {
        FILE *fp = fopen(path, "r");
        if (fp == nullptr) {
            ESP_LOGI(TAG, "SHA256 file not found at %s", path);
            return ESP_ERR_NOT_FOUND;
        }

        char hex_buf[128];
        size_t read_bytes = fread(hex_buf, 1, sizeof(hex_buf) - 1, fp);
        fclose(fp);

        if (read_bytes < 64) {
            ESP_LOGE(TAG, "SHA256 file %s is too short: %zu bytes", path, read_bytes);
            return ESP_ERR_INVALID_SIZE;
        }
        hex_buf[read_bytes] = '\0';

        // Find the first 64 non-whitespace hex characters
        const char *hex_ptr = hex_buf;
        while (*hex_ptr && isspace((unsigned char)*hex_ptr)) {
            hex_ptr++;
        }

        if (strlen(hex_ptr) < 64) {
            ESP_LOGE(TAG, "SHA256 file content is invalid");
            return ESP_ERR_INVALID_SIZE;
        }

        if (!parse_sha256_hex(hex_ptr, out)) {
            ESP_LOGE(TAG, "Failed to parse hex SHA256 from %s", path);
            return ESP_ERR_INVALID_ARG;
        }

        return ESP_OK;
    }

    // Compute SHA256 of the file content
    FILE *fp = fopen(path, "rb");
    if (fp == nullptr) {
        ESP_LOGE(TAG, "Can't open file at %s", path);
        return ESP_ERR_INVALID_STATE;
    }

    fseek(fp, 0, SEEK_END);
    size_t file_len = ftell(fp);
    rewind(fp);

    psa_hash_operation_t operation = psa_hash_operation_init();
    size_t out_len;

    if (psa_hash_setup(&operation, PSA_ALG_SHA_256) != PSA_SUCCESS) {
        ESP_LOGE(TAG, "PSA hash setup failed");
        fclose(fp);
        return ESP_FAIL;
    }

    size_t pos = 0;
    while (pos < file_len) {
        uint8_t blk[512] = {};
        size_t read_len = file_len - pos < sizeof(blk) ? file_len - pos : sizeof(blk);

        size_t actual_read = fread(blk, 1, read_len, fp);
        if (actual_read != read_len) {
            ESP_LOGE(TAG, "Failed to read %zu bytes from %s, got %zu", read_len, path, actual_read);
            fclose(fp);
            psa_hash_abort(&operation);
            return ESP_FAIL;
        }

        if (psa_hash_update(&operation, blk, actual_read) != PSA_SUCCESS) {
            ESP_LOGE(TAG, "Failed to update SHA256 digest");
            fclose(fp);
            psa_hash_abort(&operation);
            return ESP_FAIL;
        }

        pos += actual_read;
    }

    if (psa_hash_finish(&operation, out, 32, &out_len) != PSA_SUCCESS) {
        ESP_LOGE(TAG, "Failed to finalise SHA256 digest");
        psa_hash_abort(&operation);
        fclose(fp);
        return ESP_FAIL;
    }

    fclose(fp);
    return ESP_OK;
}

bool fw_asset_manager::verify_file_hash(const char *path)
{
    if (path == nullptr) {
        return false;
    }

    size_t path_len = strlen(path);
    char sidecar[96];
    if (path_len + sizeof(".sha256") > sizeof(sidecar)) {
        ESP_LOGE(TAG, "path too long for sidecar check: %s", path);
        return false;
    }
    snprintf(sidecar, sizeof(sidecar), "%s.sha256", path);

    uint8_t sha_expected[32] = {};
    esp_err_t ret = get_sha256_from_file(sidecar, sha_expected);
    if (ret == ESP_ERR_NOT_FOUND) {
        ESP_LOGI(TAG, "no sidecar for %s, hash check skipped", path);
        return true;
    }
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "failed to read/parse expected SHA256 for %s", path);
        return false;
    }

    uint8_t sha_actual[32] = {};
    if (get_sha256_from_file(path, sha_actual) != ESP_OK) {
        ESP_LOGE(TAG, "failed to compute actual SHA256 for %s", path);
        return false;
    }

    if (memcmp(sha_actual, sha_expected, 32) != 0) {
        ESP_LOGE(TAG, "SHA256 mismatch for %s!", path);
        return false;
    }

    ESP_LOGI(TAG, "SHA256 verified for %s", path);
    return true;
}

// -------------------------------------------------------------------
// Init
// -------------------------------------------------------------------

esp_err_t fw_asset_manager::verify_image_assets(const si_job_Job &new_job)
{
    // Always re-verify: files may have changed through the USB MSC window.
    if (new_job.target.has_cortex_m) {
        if (!verify_file_hash(FIRMWARE_PATH)) {
            ESP_LOGE(TAG, "init: firmware.bin SHA256 verification failed");
            return ESP_ERR_INVALID_CRC;
        }
        return ESP_OK;
    }

    const si_job_Esp32 &esp32 = new_job.target.esp32;
    for (pb_size_t i = 0; i < esp32.images_count; i++) {
        if (!verify_file_hash(esp32.images[i].path)) {
            ESP_LOGE(TAG, "init: image SHA256 verification failed: %s", esp32.images[i].path);
            return ESP_ERR_INVALID_CRC;
        }
    }
    return ESP_OK;
}

esp_err_t fw_asset_manager::init()
{
    int64_t ts = esp_timer_get_time();

    if (!verify_file_hash(JOB_PATH)) {
        ESP_LOGE(TAG, "init: job.pb SHA256 verification failed");
        return ESP_ERR_INVALID_CRC;
    }

    auto *new_job = static_cast<si_job_Job *>(heap_caps_calloc(1, sizeof(si_job_Job), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (new_job == nullptr) {
        ESP_LOGE(TAG, "init: cannot allocate %zu bytes for the job", sizeof(si_job_Job));
        return ESP_ERR_NO_MEM;
    }

    uint8_t *new_algo_bin = nullptr;
    size_t new_algo_bin_len = 0;
    auto ret = si::config::decode_job(JOB_PATH, *new_job, &new_algo_bin, &new_algo_bin_len);
    ret = ret ?: verify_image_assets(*new_job);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "init: failed to load %s: 0x%x %s", JOB_PATH, ret, esp_err_to_name(ret));
        heap_caps_free(new_algo_bin);
        heap_caps_free(new_job);
        return ret;
    }

    // Commit: drop the previous job, then publish the new config.
    heap_caps_free(algo_bin_storage);
    heap_caps_free(job);
    algo_bin_storage = new_algo_bin;
    job = new_job;
    si::config::fill_target_config(*job, algo_bin_storage, new_algo_bin_len, cfg);

    ts = esp_timer_get_time() - ts;
    ESP_LOGI(TAG, "init: OK (%lld ms): algo_bin=%zu bytes", ts / 1000, cfg.algo.algo_bin_len);
    return ESP_OK;
}

const si_job_Procedure *fw_asset_manager::non_empty(bool present, const si_job_Procedure &procedure)
{
    if (!present || procedure.steps_count == 0) {
        return nullptr;
    }
    return &procedure;
}

const si_job_Procedure *fw_asset_manager::pre_program_steps() const
{
    if (job == nullptr) {
        return nullptr;
    }
    return non_empty(job->has_pre_program, job->pre_program);
}

const si_job_Procedure *fw_asset_manager::post_program_steps() const
{
    if (job == nullptr) {
        return nullptr;
    }
    return non_empty(job->has_post_program, job->post_program);
}
