#include "fw_asset_manager.hpp"

#include <cstring>

#include <esp_heap_caps.h>
#include <esp_log.h>

#include "asset_store.hpp"
#include "config/job_decoder.hpp"

static_assert(fw_asset_manager::SHA256_LEN == asset_store::SHA256_LEN);

esp_err_t fw_asset_manager::verify_image(const char *path, const uint8_t *pinned_sha256)
{
    uint8_t actual[asset_store::SHA256_LEN] = {};
    auto ret = asset_store::hash_file(path, actual);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "verify_image: cannot read %s", path);
        return ret;
    }
    if (memcmp(actual, pinned_sha256, sizeof(actual)) != 0) {
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

esp_err_t fw_asset_manager::decode(const char *path, decoded_job &out)
{
    out.job = static_cast<si_job_Job *>(heap_caps_calloc(1, sizeof(si_job_Job), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (out.job == nullptr) {
        ESP_LOGE(TAG, "decode: cannot allocate %zu bytes for the job", sizeof(si_job_Job));
        return ESP_ERR_NO_MEM;
    }

    auto ret = si::config::decode_job(path, *out.job, &out.algo_bin, &out.algo_bin_len);
    ret = ret ?: verify_job_images(*out.job);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "decode: failed to load %s: 0x%x %s", path, ret, esp_err_to_name(ret));
        heap_caps_free(out.algo_bin);
        heap_caps_free(out.job);
        out = {};
    }
    return ret;
}

void fw_asset_manager::publish(const decoded_job &decoded, const uint8_t *sha256)
{
    heap_caps_free(algo_bin_storage);
    heap_caps_free(job);
    algo_bin_storage = decoded.algo_bin;
    job = decoded.job;
    memcpy(active_sha256, sha256, sizeof(active_sha256));
    si::config::fill_target_config(*job, algo_bin_storage, decoded.algo_bin_len, cfg);
    ESP_LOGI(TAG, "publish: '%s' active, algo_bin=%zu bytes", job_name(), cfg.algo.algo_bin_len);
}

esp_err_t fw_asset_manager::init()
{
    uint8_t sha256[asset_store::SHA256_LEN] = {};
    auto ret = asset_store::hash_file(JOB_PATH, sha256);
    if (ret == ESP_ERR_NOT_FOUND) {
        ESP_LOGW(TAG, "init: no job stored, waiting for one over SIDP");
        return ret;
    }

    decoded_job decoded = {};
    ret = ret ?: decode(JOB_PATH, decoded);
    if (ret == ESP_OK) {
        publish(decoded, sha256);
    }
    return ret;
}

esp_err_t fw_asset_manager::activate_staged(const uint8_t *sha256)
{
    // A published job is always the one saved in JOB_PATH, so this is a true no-op
    if (job != nullptr && memcmp(active_sha256, sha256, sizeof(active_sha256)) == 0) {
        return ESP_OK;
    }

    uint8_t staged[asset_store::SHA256_LEN] = {};
    if (asset_store::hash_file(STAGED_JOB_PATH, staged) != ESP_OK || memcmp(staged, sha256, sizeof(staged)) != 0) {
        ESP_LOGE(TAG, "activate_staged: no staged job with the requested hash");
        return ESP_ERR_NOT_FOUND;
    }

    decoded_job decoded = {};
    auto ret = decode(STAGED_JOB_PATH, decoded);
    if (ret != ESP_OK) {
        return ret;
    }

    // Save before publishing: a failure keeps the previous job running until reboot
    ret = asset_store::install_file(STAGED_JOB_PATH, JOB_PATH);
    if (ret != ESP_OK) {
        heap_caps_free(decoded.algo_bin);
        heap_caps_free(decoded.job);
        return ret;
    }

    publish(decoded, sha256);
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
