#include "fw_asset_manager.hpp"

#include <cstdio>
#include <cstring>
#include <unistd.h>

#include <esp_heap_caps.h>
#include <esp_log.h>
#include <esp_timer.h>

#include "asset_store.hpp"
#include "config/job_decoder.hpp"

static_assert(fw_asset_manager::SHA256_LEN == asset_store::SHA256_LEN);

esp_err_t fw_asset_manager::verify_image(const char *path, const uint8_t *pinned_sha256)
{
    uint8_t stored[asset_store::SHA256_LEN] = {};
    if (asset_store::read_sha256(path, stored) != ESP_OK) {
        ESP_LOGE(TAG, "verify_image: %s is missing", path);
        return ESP_ERR_NOT_FOUND;
    }
    if (memcmp(stored, pinned_sha256, sizeof(stored)) != 0) {
        ESP_LOGE(TAG, "verify_image: %s is not the image this job pins", path);
        return ESP_ERR_INVALID_CRC;
    }
    return ESP_OK;
}

esp_err_t fw_asset_manager::verify_job_images(const si_job_Job &new_job)
{
    if (new_job.target.has_cortex_m) {
        return verify_image(FIRMWARE_PATH, new_job.target.cortex_m.firmware_sha256);
    }

    const si_job_Esp32 &esp32 = new_job.target.esp32;
    for (pb_size_t i = 0; i < esp32.images_count; i++) {
        auto ret = verify_image(esp32.images[i].path, esp32.images[i].sha256);
        if (ret != ESP_OK) {
            return ret;
        }
    }
    return ESP_OK;
}

esp_err_t fw_asset_manager::verify_images() const
{
    if (job == nullptr) {
        return ESP_ERR_INVALID_STATE;
    }
    return verify_job_images(*job);
}

esp_err_t fw_asset_manager::load(const char *path, const uint8_t *sha256)
{
    int64_t ts = esp_timer_get_time();

    auto *new_job = static_cast<si_job_Job *>(heap_caps_calloc(1, sizeof(si_job_Job), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (new_job == nullptr) {
        ESP_LOGE(TAG, "load: cannot allocate %zu bytes for the job", sizeof(si_job_Job));
        return ESP_ERR_NO_MEM;
    }

    uint8_t *new_algo_bin = nullptr;
    size_t new_algo_bin_len = 0;
    auto ret = si::config::decode_job(path, *new_job, &new_algo_bin, &new_algo_bin_len);
    ret = ret ?: verify_job_images(*new_job);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "load: failed to load %s: 0x%x %s", path, ret, esp_err_to_name(ret));
        heap_caps_free(new_algo_bin);
        heap_caps_free(new_job);
        return ret;
    }

    // Commit: drop the previous job, then publish the new config.
    heap_caps_free(algo_bin_storage);
    heap_caps_free(job);
    algo_bin_storage = new_algo_bin;
    job = new_job;
    memcpy(active_sha256, sha256, sizeof(active_sha256));
    si::config::fill_target_config(*job, algo_bin_storage, new_algo_bin_len, cfg);

    ts = esp_timer_get_time() - ts;
    ESP_LOGI(TAG, "load: '%s' OK (%lld ms): algo_bin=%zu bytes", job_name(), ts / 1000, cfg.algo.algo_bin_len);
    return ESP_OK;
}

esp_err_t fw_asset_manager::init()
{
    uint8_t sha256[asset_store::SHA256_LEN] = {};
    if (asset_store::read_sha256(JOB_PATH, sha256) != ESP_OK) {
        ESP_LOGW(TAG, "init: no job stored, waiting for one over SIDP");
        return ESP_ERR_NOT_FOUND;
    }
    return load(JOB_PATH, sha256);
}

esp_err_t fw_asset_manager::activate_staged(const uint8_t *sha256)
{
    if (job != nullptr && memcmp(active_sha256, sha256, sizeof(active_sha256)) == 0) {
        return ESP_OK;
    }

    uint8_t staged[asset_store::SHA256_LEN] = {};
    if (asset_store::read_sha256(STAGED_JOB_PATH, staged) != ESP_OK || memcmp(staged, sha256, sizeof(staged)) != 0) {
        ESP_LOGE(TAG, "activate_staged: no staged job with the requested hash");
        return ESP_ERR_NOT_FOUND;
    }

    auto ret = load(STAGED_JOB_PATH, sha256);
    if (ret != ESP_OK) {
        return ret;
    }

    // The new job is already live; a failure here only matters after a reboot.
    ret = asset_store::install_file(STAGED_JOB_PATH, JOB_PATH, sha256);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "activate_staged: job active but not saved: 0x%x", ret);
        return ret;
    }

    char staged_sidecar[asset_store::MAX_PATH_LEN] = {};
    snprintf(staged_sidecar, sizeof(staged_sidecar), "%s.sha256", STAGED_JOB_PATH);
    unlink(staged_sidecar);
    return ESP_OK;
}

const char *fw_asset_manager::job_name() const
{
    if (job == nullptr) {
        return "";
    }
    return job->name[0] != '\0' ? job->name : job->target.variant_name;
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
