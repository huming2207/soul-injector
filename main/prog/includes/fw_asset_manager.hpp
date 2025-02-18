#pragma once

#include <cstdint>
#include <cstddef>
#include <esp_err.h>
#include <driver/gpio.h>
#include <nvs_handle.hpp>
#include "flash_algo_parser.hpp"

#define CFG_MGR_PKT_MAGIC 0x4a485349
#define CFG_MGR_FLASH_ALGO_MAX_SIZE  32768
#define CFG_MGR_FW_MAX_SIZE 1048576

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

    esp_err_t init();
    esp_err_t get_algo_bin(uint8_t *algo, size_t len, size_t *actual_len = nullptr);
    esp_err_t get_ram_size_byte(uint32_t *out) const;
    esp_err_t get_flash_size_byte(uint32_t *out);
    esp_err_t get_pc_init(uint32_t *out);
    esp_err_t get_pc_uninit(uint32_t *out);
    esp_err_t get_pc_program_page(uint32_t *out);
    esp_err_t get_pc_erase_sector(uint32_t *out);
    esp_err_t get_pc_erase_all(uint32_t *out);
    esp_err_t get_pc_verify(uint32_t *out);
    esp_err_t get_data_section_offset(uint32_t *out);
    esp_err_t get_flash_start_addr(uint32_t *out) const;
    esp_err_t get_flash_end_addr(uint32_t *out) const;
    esp_err_t get_page_size(uint32_t *out) const;
    esp_err_t get_erased_byte_val(uint32_t *out) const;
    esp_err_t get_program_page_timeout(uint32_t *out) const;
    esp_err_t get_erase_sector_timeout(uint32_t *out) const;
    esp_err_t get_sector_size(uint32_t *out) const;

    std::vector<flash_algo::test_item> &get_test_items();

public:

    /**
     * Feed in a SHA256 hash and compare with the record, to see if it's the same
     * @param sha_expected SHA2 buffer
     * @param len Length of SHA2, must be 32 bytes
     * @return true if the record SHA256 is the same as the one provided
     */
    static bool check_fw_bin_hash(uint8_t *sha_expected, size_t len);

    /**
     * Feed in a SHA256 hash and compare with the record, to see if it's the same
     * @param sha_expected SHA2 buffer
     * @param len Length of SHA2, must be 32 bytes
     * @return true if the record SHA256 is the same as the one provided
     */
    static bool check_algo_bin_hash(uint8_t *sha_expected, size_t len);

    /**
     *
     * @param path
     * @param out Output to buffer, must be 32 bytes
     * @return
     */
    static esp_err_t get_sha256_from_file(const char *path, uint8_t *out);


    static const constexpr char BASE_PATH[] = "/data";
    static const constexpr char ALGO_ELF_PATH[] = "/data/algo.elf";
    static const constexpr char FIRMWARE_PATH[] = "/data/firmware.bin";

private:
    static const constexpr char FUNC_NAME_INIT[] = "Init";
    static const constexpr char FUNC_NAME_UNINIT[] = "UnInit";
    static const constexpr char FUNC_NAME_ERASE_CHIP[] = "EraseChip";
    static const constexpr char FUNC_NAME_ERASE_SECTOR[] = "EraseSector";
    static const constexpr char FUNC_NAME_PROGRAM_PAGE[] = "ProgramPage";
    static const constexpr char FUNC_NAME_VERIFY[] = "Verify";

private:
    flash_algo::dev_description dev_descr = {};
    flash_algo::test_description test_descr = {};
    flash_algo_parser algo_parser {};
    std::vector<flash_algo::flash_sector> dev_sectors = {};
    std::vector<flash_algo::test_item> test_items = {};
    std::unique_ptr<nvs::NVSHandle> nvs_handle = {};

    static const constexpr char *TAG = "asset_mgr";

private:
    fw_asset_manager() = default;
};
