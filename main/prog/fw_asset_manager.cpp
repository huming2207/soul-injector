#include <cstring>
#include <esp_log.h>
#include <psa/crypto.h>

#include "fw_asset_manager.hpp"
#include "flash_algo_parser.hpp"

esp_err_t fw_asset_manager::init()
{
    esp_err_t ret = algo_parser.load(ALGO_ELF_PATH);
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

esp_err_t fw_asset_manager::get_algo_bin(uint8_t *algo, size_t len, size_t *actual_len, uint32_t *start_addr)
{
    return algo_parser.get_flash_algo(algo, len, actual_len, start_addr);
}

esp_err_t fw_asset_manager::get_ram_start_addr(uint32_t *out) const
{
    if (out != nullptr) {
        *out = test_descr.ram_start_addr;
    } else {
        return ESP_ERR_INVALID_ARG;
    }

    return ESP_OK;
}

esp_err_t fw_asset_manager::get_ram_size_byte(uint32_t *out) const
{
    if (out != nullptr) {
        *out = test_descr.ram_end_addr - test_descr.ram_start_addr;
    } else {
        return ESP_ERR_INVALID_ARG;
    }

    return ESP_OK;
}

esp_err_t fw_asset_manager::get_flash_size_byte(uint32_t *out)
{
    if (out != nullptr) {
        *out = dev_descr.dev_size;
    } else {
        return ESP_ERR_INVALID_ARG;
    }

    return ESP_OK;
}

esp_err_t fw_asset_manager::get_pc_init(uint32_t *out)
{
    return algo_parser.get_func_pc(FUNC_NAME_INIT, out);
}

esp_err_t fw_asset_manager::get_pc_uninit(uint32_t *out)
{
    return algo_parser.get_func_pc(FUNC_NAME_UNINIT, out);
}

esp_err_t fw_asset_manager::get_pc_program_page(uint32_t *out)
{
    return algo_parser.get_func_pc(FUNC_NAME_PROGRAM_PAGE, out);
}

esp_err_t fw_asset_manager::get_pc_erase_sector(uint32_t *out)
{
    return algo_parser.get_func_pc(FUNC_NAME_ERASE_SECTOR, out);
}

esp_err_t fw_asset_manager::get_pc_erase_all(uint32_t *out)
{
    return algo_parser.get_func_pc(FUNC_NAME_ERASE_CHIP, out);
}

esp_err_t fw_asset_manager::get_pc_verify(uint32_t *out)
{
    return algo_parser.get_func_pc(FUNC_NAME_VERIFY, out);
}

esp_err_t fw_asset_manager::get_data_section_offset(uint32_t *out)
{
    return algo_parser.get_data_section_offset(out);
}

esp_err_t fw_asset_manager::get_flash_start_addr(uint32_t *out) const
{
    if (out != nullptr) {
        *out = dev_descr.dev_addr;
    } else {
        return ESP_ERR_INVALID_ARG;
    }

    return ESP_OK;
}

esp_err_t fw_asset_manager::get_flash_end_addr(uint32_t *out) const
{
    if (out != nullptr) {
        *out = dev_descr.dev_addr + dev_descr.dev_size;
    } else {
        return ESP_ERR_INVALID_ARG;
    }

    return ESP_OK;
}

esp_err_t fw_asset_manager::get_page_size(uint32_t *out) const
{
    if (out != nullptr) {
        *out = dev_descr.page_size;
    } else {
        return ESP_ERR_INVALID_ARG;
    }

    return ESP_OK;
}

esp_err_t fw_asset_manager::get_erased_byte_val(uint32_t *out) const
{
    if (out != nullptr) {
        *out = dev_descr.empty;
    } else {
        return ESP_ERR_INVALID_ARG;
    }

    return ESP_OK;
}

esp_err_t fw_asset_manager::get_program_page_timeout(uint32_t *out) const
{
    if (out != nullptr) {
        *out = dev_descr.prog_timeout;
    } else {
        return ESP_ERR_INVALID_ARG;
    }

    return ESP_OK;
}

esp_err_t fw_asset_manager::get_erase_sector_timeout(uint32_t *out) const
{
    if (out != nullptr) {
        *out = dev_descr.erase_timeout;
    } else {
        return ESP_ERR_INVALID_ARG;
    }

    return ESP_OK;
}

esp_err_t fw_asset_manager::get_sector_size(uint32_t *out) const
{
    if (out != nullptr) {
        *out = dev_descr.page_size;
    } else {
        return ESP_ERR_INVALID_ARG;
    }

    return ESP_OK;
}

std::vector<flash_algo::test_item> &fw_asset_manager::get_test_items()
{
    return test_items;
}

bool fw_asset_manager::check_fw_bin_hash(uint8_t *sha_expected, size_t len)
{
    if (len < 32 || sha_expected == nullptr) {
        ESP_LOGE(TAG, "Invalid or incomplete SHA2! len=%u", len);
        return ESP_ERR_INVALID_ARG;
    }

    uint8_t sha_actual[32] = {};
    auto ret = get_sha256_from_file(FIRMWARE_PATH, sha_actual);
    if (ret != ESP_OK) {
        return false;
    }

    return memcmp(sha_actual, sha_expected, std::min(sizeof(sha_actual), len)) == 0;
}

bool fw_asset_manager::check_algo_bin_hash(uint8_t *sha_expected, size_t len)
{
    if (len < 32 || sha_expected == nullptr) {
        ESP_LOGE(TAG, "Invalid or incomplete SHA2! len=%u", len);
        return ESP_ERR_INVALID_ARG;
    }

    uint8_t sha_actual[32] = {};
    auto ret = get_sha256_from_file(ALGO_ELF_PATH, sha_actual);
    if (ret != ESP_OK) {
        return false;
    }

    return memcmp(sha_actual, sha_expected, std::min(sizeof(sha_actual), len)) == 0;
}

esp_err_t fw_asset_manager::get_sha256_from_file(const char *path, uint8_t *out)
{
    if (path == nullptr) {
        return ESP_ERR_INVALID_ARG;
    }

    FILE *fp = fopen(path, "r+");
    if (fp == nullptr) {
        ESP_LOGE(TAG, "Can't open file at %s", path);
        return ESP_ERR_INVALID_STATE;
    }

    fseek(fp, 0, SEEK_END);
    size_t file_len = ftell(fp);
    rewind(fp);

    psa_hash_operation_t operation = psa_hash_operation_init();
    size_t out_len;

    if (psa_hash_setup(&operation, PSA_ALG_SHA_256) != PSA_SUCCESS) {
        ESP_LOGE(TAG, "PSA hash setup failed");
        return ESP_FAIL;
    }

    size_t pos = 0;
    while (pos < file_len) {
        uint8_t blk[512] = {};
        size_t read_len = std::min(sizeof(blk), file_len - pos);

        size_t actual_read = fread(blk, 1, read_len, fp);
        if (actual_read < 1) {
            ESP_LOGE(TAG, "Failed to read stuff");
            break;
        }

        if (psa_hash_update(&operation, blk, read_len) != PSA_SUCCESS) {
            ESP_LOGE(TAG, "Failed to update SHA256 digest");
            fclose(fp);
            psa_hash_abort(&operation);
            return ESP_FAIL;
        }

        pos += actual_read;
    }

    if (psa_hash_finish(&operation, out, 32, &out_len) != PSA_SUCCESS) {
        ESP_LOGE(TAG, "Failed to finalise SHA256 digest");
        psa_hash_abort(&operation);
        return ESP_FAIL;
    }

    fclose(fp);
    return ESP_OK;
}
