#pragma once

#include <cstdint>
#include <cstddef>
#include <esp_err.h>
#include <driver/gpio.h>

#define CFG_MGR_PKT_MAGIC 0x4a485349
#define CFG_MGR_FLASH_ALGO_MAX_SIZE  32768
#define CFG_MGR_FW_MAX_SIZE 1048576

namespace cfg_def
{
    struct config_pkt
    {
        uint32_t pc_init;
        uint32_t pc_uninit;
        uint32_t pc_program_page;
        uint32_t pc_erase_sector;
        uint32_t pc_erase_all;
        uint32_t pc_verify;
        uint32_t data_section_offset;
        uint32_t flash_start_addr;
        uint32_t flash_end_addr;
        uint32_t flash_page_size;
        uint32_t erased_byte;
        uint32_t flash_sector_size;
        uint32_t program_timeout;
        uint32_t erase_timeout;
        uint32_t ram_size;
        uint32_t flash_size;
        char name[128];
    };

    struct __attribute__((packed)) algo_info
    {
        uint32_t algo_crc;
        uint32_t algo_len;
    };
}

class local_mission_manager
{
public:
    static local_mission_manager& instance()
    {
        static local_mission_manager instance;
        return instance;
    }
    local_mission_manager(local_mission_manager const &) = delete;
    void operator=(local_mission_manager const &) = delete;

    esp_err_t init();
    esp_err_t get_algo_name(char *algo_name, size_t len) const;
    esp_err_t get_target_name(char *target_name, size_t len) const;
    esp_err_t get_algo_bin(uint8_t *algo, size_t len, size_t *actual_len = nullptr) const;
    esp_err_t get_algo_bin_len(size_t *out) const;
    esp_err_t get_ram_size_byte(uint32_t *out) const;
    esp_err_t get_flash_size_byte(uint32_t *out) const;
    esp_err_t get_pc_init(uint32_t *out) const;
    esp_err_t get_pc_uninit(uint32_t *out) const;
    esp_err_t get_pc_program_page(uint32_t *out) const;
    esp_err_t get_pc_erase_sector(uint32_t *out) const;
    esp_err_t get_pc_erase_all(uint32_t *out) const;
    esp_err_t get_pc_verify(uint32_t *out) const;
    esp_err_t get_data_section_offset(uint32_t *out) const;
    esp_err_t get_flash_start_addr(uint32_t *out) const;
    esp_err_t get_flash_end_addr(uint32_t *out) const;
    esp_err_t get_page_size(uint32_t *out) const;
    esp_err_t get_erased_byte_val(uint32_t *out) const;
    esp_err_t get_program_page_timeout(uint32_t *out) const;
    esp_err_t get_erase_sector_timeout(uint32_t *out) const;
    esp_err_t get_sector_size(uint32_t *out) const;
    esp_err_t get_fw_crc(uint32_t *out) const;
    esp_err_t get_algo_crc(uint32_t *out) const;
    [[nodiscard]] bool has_valid_cfg() const;

    esp_err_t reload_cfg();

    static const constexpr char BASE_PATH[] = "/data";
    static const constexpr char ALGO_ELF_PATH[] = "/data/algo.elf";
    static const constexpr char FIRMWARE_PATH[] = "/data/fw.bin";

private:
    static const constexpr char *TAG = "mission_mgr";
    static const constexpr char *STORAGE_PARTITION_LABEL = "storage";
    bool manifest_loaded = false;
    local_mission_manager() = default;
};
