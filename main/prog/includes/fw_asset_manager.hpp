#pragma once

#include <cstdint>

#include <esp_err.h>

#include "config/target_config.hpp"

/**
 * Owns the parsed target configuration plus the one PSRAM allocation it
 * needs (the decoded flash algorithm blob), and the SHA256 sidecar
 * verification for every asset the current config references.
 *
 * Reload policy:
 *  - init() parses target.yaml; the previous algorithm blob is freed and a
 *    fresh one allocated, so at most one blob allocation is live at a time;
 *  - bootstrap_fsm forces a reload after every USB MSC exposure cycle,
 *    because that is the only window in which files on /data can change;
 *  - within one programming session the config is parsed exactly once.
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

    /**
     * Verify asset hashes and (re)parse target.yaml.
     * @param variant_name variant to select when the YAML declares several.
     */
    esp_err_t init(const char *variant_name = nullptr);

    /** Parsed configuration. Only valid after a successful init(). */
    const si::config::target_config &config() const
    {
        return cfg;
    }

    /**
     * Verify @p path against its "<path>.sha256" sidecar when the sidecar
     * exists; returns true when no sidecar is present (check skipped).
     */
    static bool verify_file_hash(const char *path);

    /** Compute SHA256 of a file, or parse the hex digest from a *.sha256 file. */
    static esp_err_t get_sha256_from_file(const char *path, uint8_t *out);

    static const constexpr char BASE_PATH[] = "/data";
    static const constexpr char TARGET_YAML_PATH[] = "/data/target.yaml";
    static const constexpr char FIRMWARE_PATH[] = "/data/firmware.bin";
    static const constexpr char TARGET_YAML_SHA256_PATH[] = "/data/target.yaml.sha256";
    static const constexpr char FIRMWARE_SHA256_PATH[] = "/data/firmware.bin.sha256";

private:
    fw_asset_manager() = default;

    esp_err_t verify_all_assets(const si::config::target_config &cfg) const;

    si::config::target_config cfg = {};
    uint8_t *algo_bin_storage = nullptr; // freed and re-allocated on each reload

    static const constexpr char *TAG = "asset_mgr";
};
