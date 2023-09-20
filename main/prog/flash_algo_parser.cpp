#include <esp_log.h>
#include <elfio/elfio_dump.hpp>
#include "flash_algo_parser.hpp"

esp_err_t flash_algo_parser::load(const char *path)
{
    if (!elf_parser.load(path)) {
        ESP_LOGE(TAG, "ELF parse failed!");
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "ELF loaded, version=%s, type=%s",
             ELFIO::dump::str_version(elf_parser.get_version()).c_str(),
             ELFIO::dump::str_type(elf_parser.get_type()).c_str()
    );

    ESP_LOGI(TAG, "ELF class=%s, OS_ABI=%s, Machine=%s",
             ELFIO::dump::str_class(elf_parser.get_class()).c_str(),
             ELFIO::dump::str_os_abi(elf_parser.get_os_abi()).c_str(),
             ELFIO::dump::str_machine(elf_parser.get_machine()).c_str()
    );

    // We probably need to enforce a check here (e.g. is it ARM or RISCV? is it a bare-metal ELF?)
    return ESP_OK;
}

esp_err_t flash_algo_parser::get_test_description(flash_algo::test_description *descr)
{
    return get_section_data(descr, "SelfTestInfo", sizeof(flash_algo::test_description), nullptr);
}

esp_err_t flash_algo_parser::get_dev_description(flash_algo::dev_description *descr)
{
    return get_section_data(descr, "DeviceData", sizeof(flash_algo::dev_description), nullptr);
}

esp_err_t flash_algo_parser::get_flash_algo(uint8_t *buf_out, size_t buf_len, size_t *actual_len)
{
    return get_section_data(buf_out, "PrgCode", buf_len, actual_len);
}

esp_err_t flash_algo_parser::get_section_data(void *data_out, const char *section_name, size_t min_size, size_t *actual_size) const
{
    if (data_out == nullptr) {
        return ESP_ERR_INVALID_ARG;
    }

    auto section_cnt = elf_parser.sections.size();
    if (section_cnt < 1) {
        ESP_LOGE(TAG, "No section at all?");
        return ESP_ERR_INVALID_SIZE;
    }

    for (size_t idx = 0; idx < section_cnt; idx += 1) {
        auto curr_section = elf_parser.sections[idx];
        if ( curr_section->get_type() == ELFIO::SHT_NOBITS ) {
            continue;
        }

        if (curr_section->get_name() == section_name) {
            memcpy(data_out, curr_section->get_data(), std::min((size_t)curr_section->get_size(), min_size));
            if (actual_size != nullptr) {
                *actual_size = curr_section->get_size();
            }

            return ESP_OK;
        } else {
            continue;
        }
    }

    ESP_LOGE(TAG, "Section '%s' not found!", section_name);
    return ESP_ERR_NOT_FOUND;
}

