#include <cstring>
#include <esp_log.h>
#include <esp_crc.h>

#include "local_mission_manager.hpp"
#include "file_utils.hpp"
#include "flash_algo_parser.hpp"

esp_err_t local_mission_manager::init()
{
    esp_err_t ret = ESP_OK;

    ret = algo_parser.load(ALGO_ELF_PATH);
    if (ret != ESP_OK) {
        return ret;
    }

    ret = algo_parser.get_dev_description(&dev_descr, dev_sectors);
    if (ret != ESP_OK) {
        return ret;
    }

    ESP_LOGI(TAG, "Dev: %s; addr: 0x%08lx, size: %lu, page size %lu", dev_descr.dev_name, dev_descr.dev_addr, dev_descr.dev_size, dev_descr.page_size);

    ret = algo_parser.get_test_description(&test_descr, test_items);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "No test info found, probably generic flash algo? 0x%x", ret);
    }

    return ESP_OK;
}

esp_err_t local_mission_manager::get_algo_bin(uint8_t *algo, size_t len, size_t *actual_len)
{
    return algo_parser.get_flash_algo(algo, len, actual_len);
}

esp_err_t local_mission_manager::get_algo_bin_len(size_t *out)
{
    return ESP_OK;
}

esp_err_t local_mission_manager::get_ram_size_byte(uint32_t *out)
{
    return ESP_OK;
}

esp_err_t local_mission_manager::get_flash_size_byte(uint32_t *out)
{
    return ESP_OK;
}

esp_err_t local_mission_manager::get_pc_init(uint32_t *out)
{
    return algo_parser.get_func_pc(FUNC_NAME_INIT, out);
}

esp_err_t local_mission_manager::get_pc_uninit(uint32_t *out)
{
    return algo_parser.get_func_pc(FUNC_NAME_UNINIT, out);
}

esp_err_t local_mission_manager::get_pc_program_page(uint32_t *out)
{
    return algo_parser.get_func_pc(FUNC_NAME_PROGRAM_PAGE, out);
}

esp_err_t local_mission_manager::get_pc_erase_sector(uint32_t *out)
{
    return algo_parser.get_func_pc(FUNC_NAME_ERASE_SECTOR, out);
}

esp_err_t local_mission_manager::get_pc_erase_all(uint32_t *out)
{
    return algo_parser.get_func_pc(FUNC_NAME_ERASE_CHIP, out);
}

esp_err_t local_mission_manager::get_pc_verify(uint32_t *out)
{
    return algo_parser.get_func_pc(FUNC_NAME_VERIFY, out);
}

esp_err_t local_mission_manager::get_data_section_offset(uint32_t *out)
{
    return algo_parser.get_data_section_offset(out);
}

esp_err_t local_mission_manager::get_flash_start_addr(uint32_t *out)
{
    return ESP_OK;
}

esp_err_t local_mission_manager::get_flash_end_addr(uint32_t *out)
{
    return ESP_OK;
}

esp_err_t local_mission_manager::get_page_size(uint32_t *out)
{
    return ESP_OK;
}

esp_err_t local_mission_manager::get_erased_byte_val(uint32_t *out)
{
    return ESP_OK;
}

esp_err_t local_mission_manager::get_program_page_timeout(uint32_t *out)
{
    return ESP_OK;
}

esp_err_t local_mission_manager::get_erase_sector_timeout(uint32_t *out)
{
    return ESP_OK;
}

esp_err_t local_mission_manager::get_sector_size(uint32_t *out)
{
    return ESP_OK;
}

esp_err_t local_mission_manager::get_fw_crc(uint32_t *out)
{
    return ESP_OK;
}

esp_err_t local_mission_manager::get_algo_crc(uint32_t *out)
{
    return ESP_OK;
}
