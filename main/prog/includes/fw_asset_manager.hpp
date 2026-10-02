#pragma once

#include <cstdint>

#include <esp_err.h>

#include <job.pb.h>

#include "config/target_config.hpp"

/**
 * Owns the decoded programming job (/data/job.pb, compiled on the host by
 * `sidp-agent compile`), the target configuration derived from it, and the
 * SHA256 sidecar verification for every asset the job references.
 *
 * Memory: the decoded job and the flash algorithm blob are each one PSRAM
 * heap allocation, made only when assets are loaded.
 *
 * Reload policy:
 *  - init() decodes into fresh allocations and swaps them in on success, so
 *    a failed reload leaves the previous job untouched;
 *  - bootstrap_fsm forces a reload after every USB MSC exposure cycle,
 *    because that is the only window in which files on /data can change;
 *  - within one programming session the job is decoded exactly once.
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

    /** Verify asset hashes and (re)load the job. */
    esp_err_t init();

    /** Target configuration. Only valid after a successful init(). */
    const si::config::target_config &config() const
    {
        return cfg;
    }

    /** Steps run before target detection, or nullptr when there are none. */
    const si_job_Procedure *pre_program_steps() const;

    /** Steps run after self tests, or nullptr when there are none. */
    const si_job_Procedure *post_program_steps() const;

    /**
     * Verify @p path against its "<path>.sha256" sidecar when the sidecar
     * exists; returns true when no sidecar is present (check skipped).
     */
    static bool verify_file_hash(const char *path);

    /** Compute SHA256 of a file, or parse the hex digest from a *.sha256 file. */
    static esp_err_t get_sha256_from_file(const char *path, uint8_t *out);

    static const constexpr char BASE_PATH[] = "/data";
    static const constexpr char JOB_PATH[] = "/data/job.pb";
    static const constexpr char FIRMWARE_PATH[] = "/data/firmware.bin";

private:
    fw_asset_manager() = default;

    static esp_err_t verify_image_assets(const si_job_Job &new_job);
    static const si_job_Procedure *non_empty(bool present, const si_job_Procedure &procedure);

    si::config::target_config cfg = {};
    si_job_Job *job = nullptr;           // PSRAM; replaced on each successful reload
    uint8_t *algo_bin_storage = nullptr; // PSRAM; replaced on each successful reload

    static const constexpr char *TAG = "asset_mgr";
};
