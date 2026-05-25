#pragma once

#include <cstdint>
#include <cstddef>
#include <vector>
#include <memory>
#include <esp_err.h>
#include <driver/gpio.h>
#include <nvs_handle.hpp>
#include "flash_algo_parser.hpp"

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
    esp_err_t get_algo_bin(uint8_t *algo, size_t len, size_t *actual_len = nullptr, uint32_t *code_start_addr = nullptr);
    esp_err_t get_ram_start_addr(uint32_t *out) const;
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

    static bool check_fw_bin_hash(uint8_t *sha_expected, size_t len);
    static bool check_algo_bin_hash(uint8_t *sha_expected, size_t len);
    static esp_err_t get_sha256_from_file(const char *path, uint8_t *out);

    static const constexpr char BASE_PATH[] = "/data";
    static const constexpr char TARGET_YAML_PATH[] = "/data/target.yaml";
    static const constexpr char FIRMWARE_PATH[] = "/data/firmware.bin";

private:
    // Decoded flash algorithm binary
    uint8_t *algo_bin = nullptr;
    size_t algo_bin_len = 0;

    // From flash algorithm YAML entry
    uint32_t load_addr = 0;
    uint32_t pc_init_val = 0;
    uint32_t pc_uninit_val = 0;
    uint32_t pc_program_page_val = 0;
    uint32_t pc_erase_sector_val = 0;
    uint32_t pc_erase_all_val = 0;
    uint32_t pc_verify_val = 0;
    uint32_t data_section_offset_val = 0;

    // From flash_properties
    uint32_t flash_addr_start = 0;
    uint32_t flash_addr_end = 0;
    uint32_t page_sz = 0;
    uint32_t erased_byte = 0;
    uint32_t prog_timeout = 0;
    uint32_t erase_timeout = 0;

    // From memory_map Ram regions
    uint32_t ram_start = 0;
    uint32_t ram_end = 0;

    // Self tests
    std::vector<flash_algo::test_item> test_items;

    std::unique_ptr<nvs::NVSHandle> nvs_handle = {};

    static const constexpr char *TAG = "asset_mgr";

private:
    fw_asset_manager() = default;
};
