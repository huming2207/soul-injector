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

    return run_elf_check();
}

esp_err_t flash_algo_parser::load(const uint8_t *buf, size_t len)
{
    imemstream stream(reinterpret_cast<const char *>(buf), len);

    if (!elf_parser.load(stream)) {
        ESP_LOGE(TAG, "ELF parse failed!");
        return ESP_FAIL;
    }

    return run_elf_check();
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
        item_ptr += sizeof(flash_algo::test_item);
    }

    free(items_buf);

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
    ret = get_section_data(items_buf, "DeviceData", item_size, &actual_items_size, sizeof(flash_algo::dev_description));
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to get sector info: 0x%x", ret);
        return ret;
    }

    ESP_LOGD(TAG, "sizeof(flash_algo::test_description) = %zu", sizeof(flash_algo::dev_description));
    auto item_left_size = (int32_t)(actual_items_size);
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
        item_ptr += sizeof(flash_algo::flash_sector);
    }

    free(items_buf);

    return ret;
}

esp_err_t flash_algo_parser::get_flash_algo(uint8_t *buf_out, size_t buf_len, size_t *actual_len)
{
    size_t out_len = 0, curr_pos = 0;
    auto ret = get_section_data(buf_out, ALGO_BIN_CODE_SECTION_NAME, buf_len - curr_pos, &out_len, 0);

    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to get section data for PrgCode: 0x%x", ret);
        return ret;
    }

    if (curr_pos > buf_len) {
        ESP_LOGW(TAG, "No space left for for PrgCode: buf @ %p, got %u, need %u", buf_out, buf_len, curr_pos);
        return ESP_ERR_NO_MEM;
    }

    ESP_LOGI(TAG, "Got PrgCode length: %u", out_len);

    curr_pos += out_len;
    ret = ret ?: get_section_data(buf_out + curr_pos, ALGO_BIN_DATA_SECTION_NAME, buf_len - curr_pos, &out_len, 0);

    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to get section data for PrgData: 0x%x", ret);
        return ret;
    }

    if (curr_pos > buf_len) {
        ESP_LOGW(TAG, "No space left for PrgData: buf @ %p, got %u, need %u", buf_out, buf_len, curr_pos);
        return ESP_ERR_NO_MEM;
    }

    ESP_LOGI(TAG, "Got PrgData length: %u", out_len);

    curr_pos += out_len;
    size_t bss_len = 0;
    ret = ret ?: get_section_length(ALGO_BIN_BSS_SECTION_NAME, &bss_len);

    if (curr_pos + bss_len > buf_len) {
        ESP_LOGE(TAG, "Insufficient place for BSS, need %u, got only %u", curr_pos + bss_len, buf_len);
    }

    memset(buf_out + curr_pos, 0, bss_len); // Wipe the RAM for BSS
    ESP_LOGI(TAG, "Zeroed BSS length = %u", bss_len);

    return ret;
}

esp_err_t flash_algo_parser::get_section_data(void *data_out, const char *section_name, size_t min_size, size_t *actual_size, uint32_t offset) const
{
    if (section_name == nullptr) {
        return ESP_ERR_INVALID_ARG;
    }

    auto section_cnt = elf_parser.sections.size();
    if (section_cnt < 1) {
        ESP_LOGE(TAG, "No section at all?");
        return ESP_ERR_INVALID_SIZE;
    }

    for (size_t idx = 0; idx < section_cnt; idx += 1) {
        auto curr_section = elf_parser.sections[idx];
        if (curr_section->get_type() != ELFIO::SHT_PROGBITS) {
            continue;
        }

        if (curr_section->get_name() == section_name) {
            if (data_out != nullptr) {
                memcpy(data_out, curr_section->get_data() + offset, std::min((size_t)(curr_section->get_size() - offset), min_size));
            }

            if (actual_size != nullptr) {
                *actual_size = curr_section->get_size() - offset;
            }

            return ESP_OK;
        } else {
            continue;
        }
    }

    ESP_LOGE(TAG, "Section '%s' not found!", section_name);
    return ESP_ERR_NOT_FOUND;
}

esp_err_t flash_algo_parser::get_section_length(const char *section_name, size_t *len_out, ELFIO::Elf_Word type) const
{
    if (len_out == nullptr) {
        return ESP_ERR_INVALID_ARG;
    }

    auto section_cnt = elf_parser.sections.size();
    if (section_cnt < 1) {
        ESP_LOGE(TAG, "No section at all?");
        return ESP_ERR_INVALID_SIZE;
    }

    for (size_t idx = 0; idx < section_cnt; idx += 1) {
        auto curr_section = elf_parser.sections[idx];
        if (curr_section->get_type() != type) {
            continue;
        }

        if (curr_section->get_name() == section_name) {
            *len_out = curr_section->get_size();
            return ESP_OK;
        } else {
            continue;
        }
    }

    ESP_LOGE(TAG, "BSS '%s' not found!", section_name);
    return ESP_ERR_NOT_FOUND;
}

esp_err_t flash_algo_parser::get_section_addr(const char *section_name, uint32_t *addr_out, ELFIO::Elf_Word type) const
{
    if (addr_out == nullptr) {
        return ESP_ERR_INVALID_ARG;
    }

    auto section_cnt = elf_parser.sections.size();
    if (section_cnt < 1) {
        ESP_LOGE(TAG, "No section at all?");
        return ESP_ERR_INVALID_SIZE;
    }

    for (size_t idx = 0; idx < section_cnt; idx += 1) {
        auto curr_section = elf_parser.sections[idx];
        if (curr_section->get_type() != type) {
            continue;
        }

        if (curr_section->get_name() == section_name) {
            *addr_out = (uint32_t)(curr_section->get_address());
            return ESP_OK;
        } else {
            continue;
        }
    }

    ESP_LOGE(TAG, "BSS '%s' not found!", section_name);
    return ESP_ERR_NOT_FOUND;
}

esp_err_t flash_algo_parser::get_func_pc(const char *func_name, uint32_t *pc_out)
{
    if (func_name == nullptr || pc_out == nullptr) {
        return ESP_ERR_INVALID_ARG;
    }

    auto section_cnt = elf_parser.sections.size();
    if (section_cnt < 1) {
        ESP_LOGE(TAG, "No section at all?");
        return ESP_ERR_INVALID_SIZE;
    }

    for (size_t idx = 0; idx < section_cnt; idx += 1) {
        auto curr_section = elf_parser.sections[idx];
        if (curr_section->get_type() != ELFIO::SHT_SYMTAB) {
            ESP_LOGD(TAG, "Skipping non-symbol section: 0x%lx", curr_section->get_type());
            continue;
        }

        ELFIO::const_symbol_section_accessor symbols(elf_parser, curr_section);
        auto sym_cnt = symbols.get_symbols_num();
        if (unlikely(sym_cnt > SIZE_MAX)) {
            ESP_LOGE(TAG, "Symbol table found but too huge");
            return ESP_ERR_NO_MEM;
        }

        for (size_t sym_idx = 0; sym_idx < sym_cnt; sym_idx += 1) {
            std::string name;
            ELFIO::Elf64_Addr value = 0;
            ELFIO::Elf_Xword size = 0;
            unsigned char bind = 0;
            unsigned char type = 0;
            ELFIO::Elf_Half section = 0;
            unsigned char other = 0;
            if (!symbols.get_symbol(sym_idx, name, value, size, bind, type, section, other)) {
                ESP_LOGD(TAG, "Failed to get symbol (corrupted ELF?)");
                continue;
            }

            if (type != ELFIO::STT_FUNC) {
                ESP_LOGD(TAG, "This symbol 0x%x isn't a function", type);
                continue;
            }

            if (name == func_name) {
                *pc_out = (uint32_t)(value & 0xffffffffULL);
                return ESP_OK;
            } else {
                ESP_LOGD(TAG, "Name mismatch: %s", name.c_str());
            }
        }
    }

    ESP_LOGE(TAG, "Function '%s' not found", func_name);
    return ESP_ERR_NOT_FOUND;
}

esp_err_t flash_algo_parser::get_data_section_offset(uint32_t *offset)
{
    if (offset == nullptr) {
        return ESP_ERR_INVALID_ARG;
    }

    // Two ways to do this: 1. try if we can get PrgData's address
    uint32_t data_section_addr = 0;
    auto ret = get_section_addr(ALGO_BIN_DATA_SECTION_NAME, &data_section_addr);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "No data section found, assuming it's after code section");
    } else {
        ESP_LOGI(TAG, "Data section found at 0x%08lx", data_section_addr);
        *offset = data_section_addr;
        return ret;
    }

    // 2. ...or if no PrgData, then we assume PrgData is right after the code section
    uint32_t code_section_addr = 0;
    size_t code_section_len = 0;
    ret = get_section_addr(ALGO_BIN_CODE_SECTION_NAME, &code_section_addr);
    ret = ret ?: get_section_length(ALGO_BIN_CODE_SECTION_NAME, &code_section_len);

    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to get data section offset: 0x%x", ret);
    } else {
        data_section_addr = code_section_addr + code_section_len;
        ESP_LOGI(TAG, "Assuming data section at 0x%08lx", data_section_addr);
        *offset = data_section_addr;
    }

    return ret;
}

esp_err_t flash_algo_parser::run_elf_check()
{
    // We probably need to enforce a check here (e.g. is it ARM or RISCV? is it a bare-metal ELF?)
    if (elf_parser.get_class() == ELFIO::ELFCLASS64) {
        ESP_LOGE(TAG, "No 64-bit support for now");
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

    return ESP_OK;
}



