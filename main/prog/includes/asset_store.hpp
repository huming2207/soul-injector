#pragma once

#include <cstddef>
#include <cstdint>
#include <cstdio>

#include <esp_err.h>
#include <psa/crypto.h>

/**
 * Files under /data, uploaded in chunks by the SIDP management service.
 *
 * An upload is written to "<name>.part" and hashed as it arrives. Commit
 * checks the hash and replaces "<name>". No hash is stored with the file:
 * whoever needs to trust a file hashes its contents (hash_file()).
 *
 * Names are lowercase only, since FAT would treat other spellings as the
 * same file.
 *
 * One upload at a time. Its state lives in RAM: a device reset restarts the
 * upload from zero, and remove_stale_uploads() drops what was left behind.
 * Only the SIDP service task calls the upload methods.
 */
class asset_store
{
public:
    static asset_store *instance()
    {
        static asset_store _instance;
        return &_instance;
    }

    asset_store(asset_store const &) = delete;
    void operator=(asset_store const &) = delete;

    /**
     * Start (or resume) uploading @p name.
     *
     * If the same name, size and hash are already being uploaded, the upload
     * resumes at @p offset_out. If the device already holds exactly this
     * asset, @p present_out is set and nothing needs to be sent.
     */
    esp_err_t begin(const char *name, uint32_t size, const uint8_t *sha256, bool *present_out, uint32_t *offset_out);

    /** Append @p len bytes; @p offset must equal the bytes received so far. */
    esp_err_t write(uint32_t offset, const uint8_t *data, size_t len);

    /** Check the hash and replace the asset. Must not run during a programming run. */
    esp_err_t commit();

    /** SHA-256 of the contents of @p path. ESP_ERR_NOT_FOUND when there is no such file. */
    static esp_err_t hash_file(const char *path, uint8_t *out);

    /** Replace @p to with @p from. */
    static esp_err_t install_file(const char *from, const char *to);

    /** Delete partial uploads left by a reset. Call once at boot, before any upload. */
    static void remove_stale_uploads();

    static bool is_valid_name(const char *name);

    static const constexpr size_t SHA256_LEN = 32;
    static const constexpr size_t MAX_NAME_LEN = 31;
    static const constexpr size_t MAX_PATH_LEN = 64;
    static const constexpr char BASE_PATH[] = "/data";
    static const constexpr char JOB_NAME[] = "job.pb";

private:
    asset_store() = default;

    void abort_upload();
    bool is_resumable(const char *new_name, uint32_t new_size, const uint8_t *sha256) const;
    static bool is_present(const char *path, uint32_t expect_size, const uint8_t *sha256);
    static bool make_path(char *out, const char *prefix, const char *file_name, const char *suffix);

    FILE *fp = nullptr;
    psa_hash_operation_t hash = PSA_HASH_OPERATION_INIT;
    char name[MAX_NAME_LEN + 1] = {};
    uint8_t expected_sha256[SHA256_LEN] = {};
    uint32_t size = 0;
    uint32_t written = 0;

    static const constexpr char DIR_PREFIX[] = "/data/";
    static const constexpr char PART_SUFFIX[] = ".part";
    static const constexpr char *TAG = "asset_store";
};
