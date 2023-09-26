#include <cstring>
#include <esp_log.h>
#include <esp_crc.h>

#include "local_mission_manager.hpp"
#include "file_utils.hpp"

esp_err_t local_mission_manager::init()
{
    esp_err_t ret = ESP_OK;


    return ret;
}

esp_err_t local_mission_manager::get_algo_name(char *algo_name, size_t len) const
{
    return ESP_OK;
}

esp_err_t local_mission_manager::get_target_name(char *target_name, size_t len) const
{
    return ESP_OK;
}

esp_err_t local_mission_manager::get_algo_bin(uint8_t *algo, size_t len, size_t *actual_len) const
{
    return ESP_OK;
}

esp_err_t local_mission_manager::get_algo_bin_len(size_t *out) const
{
    return ESP_OK;
}

esp_err_t local_mission_manager::get_ram_size_byte(uint32_t *out) const
{
    return ESP_OK;
}

esp_err_t local_mission_manager::get_flash_size_byte(uint32_t *out) const
{
    return ESP_OK;
}

esp_err_t local_mission_manager::get_pc_init(uint32_t *out) const
{
    return ESP_OK;
}

esp_err_t local_mission_manager::get_pc_uninit(uint32_t *out) const
{
    return ESP_OK;
}

esp_err_t local_mission_manager::get_pc_program_page(uint32_t *out) const
{
    return ESP_OK;
}

esp_err_t local_mission_manager::get_pc_erase_sector(uint32_t *out) const
{
    return ESP_OK;
}

esp_err_t local_mission_manager::get_pc_erase_all(uint32_t *out) const
{
    return ESP_OK;
}

esp_err_t local_mission_manager::get_pc_verify(uint32_t *out) const
{
    return ESP_OK;
}

esp_err_t local_mission_manager::get_data_section_offset(uint32_t *out) const
{
    return ESP_OK;
}

esp_err_t local_mission_manager::get_flash_start_addr(uint32_t *out) const
{
    return ESP_OK;
}

esp_err_t local_mission_manager::get_flash_end_addr(uint32_t *out) const
{
    return ESP_OK;
}

esp_err_t local_mission_manager::get_page_size(uint32_t *out) const
{
    return ESP_OK;
}

esp_err_t local_mission_manager::get_erased_byte_val(uint32_t *out) const
{
    return ESP_OK;
}

esp_err_t local_mission_manager::get_program_page_timeout(uint32_t *out) const
{
    return ESP_OK;
}

esp_err_t local_mission_manager::get_erase_sector_timeout(uint32_t *out) const
{
    return ESP_OK;
}

esp_err_t local_mission_manager::get_sector_size(uint32_t *out) const
{
    return ESP_OK;
}

esp_err_t local_mission_manager::get_fw_crc(uint32_t *out) const
{
    return ESP_OK;
}

esp_err_t local_mission_manager::get_algo_crc(uint32_t *out) const
{
    return ESP_OK;
}

esp_err_t local_mission_manager::reload_cfg()
{
    return ESP_OK;
}

bool local_mission_manager::has_valid_cfg() const
{
    return manifest_loaded;
}
