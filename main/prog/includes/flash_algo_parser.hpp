#pragma once

#include <cstdint>
#include <vector>
#include <esp_err.h>

namespace flash_algo
{
    enum self_test_type : uint8_t
    {
        INTERNAL_SIMPLE_TEST = 0,
        INTERNAL_EXTEND_TEST = 1,
        EXTERNAL_TEST = 2,
        END_MARK = 0xff, // Marks the end of the test descriptor array, behaves like null terminator for string
    };

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
        self_test_type type;
        uint16_t id;
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
