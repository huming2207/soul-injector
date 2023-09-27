#include <esp_log.h>
#include <esp_heap_caps.h>
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

esp_err_t flash_algo_parser::get_test_description(flash_algo::test_description *descr, std::vector<flash_algo::test_item> &test_items)
{
    size_t actual_size = 0;
    esp_err_t ret = get_section_data(descr, "SelfTestInfo", sizeof(flash_algo::test_description), &actual_size, 0);
    if (ret != ESP_OK) {
        return ret;
    }

    auto item_size = (int32_t)(actual_size - sizeof(flash_algo::test_description));
    ESP_LOGI(TAG, "Found self test info, len=%d; RAM start=0x%08lx, end=0x%08lx; test count=%lu; item size=%ld",
             actual_size, descr->ram_start_addr, descr->ram_end_addr, descr->test_cnt, item_size);

    if (item_size < 0) {
        ESP_LOGW(TAG, "No valid test item found!");
        return ret;
    }

    auto *items_buf = (uint8_t *)heap_caps_calloc(1, item_size, MALLOC_CAP_SPIRAM);
    if (items_buf == nullptr) {
        ESP_LOGE(TAG, "Failed to prepare test item buffer");
        return ESP_ERR_NO_MEM;
    }

    size_t actual_items_size = 0;
    ret = get_section_data(items_buf, "SelfTestInfo", item_size, &actual_items_size, sizeof(flash_algo::test_description));
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "Failed to get self test info: 0x%x", ret);
        return ret;
    }

    auto item_left_size = (int32_t)actual_items_size;
    uint8_t *item_ptr = items_buf;
    test_items.clear();

    while (item_left_size > sizeof(flash_algo::test_item)) {
        flash_algo::test_item item = {};
        memcpy(&item, item_ptr, sizeof(flash_algo::test_item));
        item.name[sizeof(item.name) - 1] = '\0';

        if (item.id == UINT32_MAX) {
            ESP_LOGI(TAG, "End of test item detected, got %u items", test_items.size());
            break;
        }

        ESP_LOGI(TAG, "Found test item ID=%ld: \"%s\"", item.id, item.name);
        test_items.emplace_back(item);

        item_left_size -= sizeof(flash_algo::test_item);
        items_buf += sizeof(flash_algo::test_item);
    }

    return ret;
}

esp_err_t flash_algo_parser::get_dev_description(flash_algo::dev_description *descr, std::vector<flash_algo::flash_sector> &sectors)
{
    size_t actual_size = 0;
    esp_err_t ret = get_section_data(descr, "DeviceData", sizeof(flash_algo::dev_description), &actual_size, 0);
    if (ret != ESP_OK) {
        return ret;
    }

    auto item_size = (int32_t)(actual_size - sizeof(flash_algo::dev_description));
    ESP_LOGI(TAG, "Found device %s, len=%d; page size=%ld, device size=%ld; program timeout=%lu; item size=%ld",
             descr->dev_name, actual_size, descr->page_size, descr->dev_size, descr->prog_timeout, item_size);

    if (item_size < 0) {
        ESP_LOGW(TAG, "No valid sector item found!");
        return ret;
    }

    auto *items_buf = (uint8_t *)heap_caps_calloc(1, item_size, MALLOC_CAP_SPIRAM);
    if (items_buf == nullptr) {
        ESP_LOGE(TAG, "Failed to prepare test item buffer");
        return ESP_ERR_NO_MEM;
    }

    size_t actual_items_size = 0;
    ret = get_section_data(items_buf, "DeviceData", item_size, &actual_items_size, sizeof(flash_algo::test_description));
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to get sector info: 0x%x", ret);
        return ret;
    }

    auto item_left_size = (int32_t)actual_items_size;
    uint8_t *item_ptr = items_buf;
    sectors.clear();

    while (item_left_size > sizeof(flash_algo::flash_sector)) {
        flash_algo::flash_sector item = {};
        memcpy(&item, item_ptr, sizeof(flash_algo::flash_sector));

        if (item.size == UINT32_MAX || item.addr == UINT32_MAX) {
            ESP_LOGI(TAG, "End of sector item detected, got %u items", sectors.size());
            break;
        }

        ESP_LOGI(TAG, "Found sector item @ 0x%08lx, len=%lu", item.addr, item.size);
        sectors.emplace_back(item);

        item_left_size -= sizeof(flash_algo::flash_sector);
        items_buf += sizeof(flash_algo::flash_sector);
    }

    return ret;
}

esp_err_t flash_algo_parser::get_flash_algo(uint8_t *buf_out, size_t buf_len, size_t *actual_len)
{
    return get_section_data(buf_out, "PrgCode", buf_len, actual_len, 0);
}

esp_err_t flash_algo_parser::get_section_data(void *data_out, const char *section_name, size_t min_size, size_t *actual_size, uint32_t offset) const
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
            memcpy(data_out, curr_section->get_data() + offset, std::min((size_t)(curr_section->get_size() - offset), min_size));
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

