#pragma once

#include <cstdint>
#include <vector>
#include <esp_err.h>
#include <elfio/elfio.hpp>

namespace flash_algo
{
    struct __attribute__((packed)) flash_sector
    {
        uint32_t size;
        uint32_t addr;
    };

    struct __attribute__((packed)) dev_description
    {
        uint16_t version;
        char dev_name[128];
        uint16_t dev_type;
        uint32_t dev_addr;
        uint32_t dev_size;
        uint32_t page_size;
        uint32_t _reserved;
        uint8_t empty;
        uint32_t prog_timeout;
        uint32_t erase_timeout;
    };

    struct __attribute__((packed)) test_item
    {
        uint32_t id;
        char name[32];
    };

    struct __attribute__((packed)) test_description
    {
        uint32_t magic;
        uint32_t ram_start_addr;
        uint32_t ram_end_addr;
        uint32_t test_cnt;
    };

    static const constexpr uint32_t SELF_TEST_MAGIC = 0x536f756c; // "Soul"
}

class flash_algo_parser
{
public:
    esp_err_t load(const char *path);
    esp_err_t get_test_description(flash_algo::test_description *descr, std::vector<flash_algo::test_item> &test_items);
    esp_err_t get_dev_description(flash_algo::dev_description *descr, std::vector<flash_algo::flash_sector> &sectors);
    esp_err_t get_flash_algo(uint8_t *buf_out, size_t buf_len, size_t *actual_len);

private:
    esp_err_t get_section_data(void *data_out, const char *section_name, size_t min_size, size_t *actual_size, uint32_t offset) const;

private:
    ELFIO::elfio elf_parser;

private:
    static const constexpr char TAG[] = "algo_parser";
};
