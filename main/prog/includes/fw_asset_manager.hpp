#pragma once

#include <cstddef>
#include <cstdint>

#include <esp_err.h>

#include <job.pb.h>

#include "config/target_config.hpp"

/**
 * Owns the active programming job (/data/job.pb, compiled on the host by
 * `sidp-agent compile`) and the target configuration derived from it.
 *
 * The device keeps exactly one job. A new job is uploaded as asset
 * "job.pb.new" and only replaces /data/job.pb through activate_staged(),
 * after it decodes, validates and every image it pins is present with the
 * pinned SHA-256.
 *
 * Memory: the decoded job and the flash algorithm blob are each one PSRAM
 * heap allocation, replaced only when a job is (re)loaded.
 *
 * Thread safety: job_controller serialises loading against programming runs;
 * the programming task reads the job only while no load can happen.
 */
class fw_asset_manager
{
public:
    static fw_asset_manager *instance()
    {
        static fw_asset_manager _instance;
        return &_instance;
    }

    fw_asset_manager(fw_asset_manager const &) = delete;
    void operator=(fw_asset_manager const &) = delete;

    /** Load /data/job.pb at boot. ESP_ERR_NOT_FOUND when no job is stored. */
    esp_err_t init();

    /**
     * Make the staged job with @p sha256 the active job. Already active:
     * nothing happens. A failure leaves the previous job in place.
     */
    esp_err_t activate_staged(const uint8_t *sha256);

    /** Check every image the active job pins against its stored hash. */
    esp_err_t verify_images() const;

    bool has_job() const
    {
        return job != nullptr;
    }

    /** Job name, falling back to the variant name. Only valid with has_job(). */
    const char *job_name() const;

    /** SHA-256 of the active job file. Only valid with has_job(). */
    const uint8_t *job_sha256() const
    {
        return active_sha256;
    }

    /** Target configuration. Only valid with has_job(). */
    const si::config::target_config &config() const
    {
        return cfg;
    }

    /** Steps run before target detection, or nullptr when there are none. */
    const si_job_Procedure *pre_program_steps() const;

    /** Steps run after self tests, or nullptr when there are none. */
    const si_job_Procedure *post_program_steps() const;

    static const constexpr char JOB_PATH[] = "/data/job.pb";
    static const constexpr char STAGED_JOB_PATH[] = "/data/job.pb.new";
    static const constexpr char FIRMWARE_PATH[] = "/data/firmware.bin";
    static const constexpr size_t SHA256_LEN = 32;

private:
    fw_asset_manager() = default;

    esp_err_t load(const char *path, const uint8_t *sha256);
    static esp_err_t verify_image(const char *path, const uint8_t *pinned_sha256);
    static esp_err_t verify_job_images(const si_job_Job &new_job);
    static const si_job_Procedure *non_empty(bool present, const si_job_Procedure &procedure);

    si::config::target_config cfg = {};
    si_job_Job *job = nullptr;           // PSRAM; replaced on each successful load
    uint8_t *algo_bin_storage = nullptr; // PSRAM; replaced on each successful load
    uint8_t active_sha256[SHA256_LEN] = {};

    static const constexpr char *TAG = "asset_mgr";
};
