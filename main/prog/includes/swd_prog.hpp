#pragma once

#include <esp_err.h>
#include <swd_host.h>
#include <led_ctrl.hpp>
#include "config_manager.hpp"

namespace swd_def
{
    enum state : uint8_t
    {
        UNKNOWN = 0,
        INITIALISED = 1,
        FLASH_ALG_LOADED = 2,
        FLASH_ALG_INITED = 3,
        FLASH_ALG_RUNNING = 4,
        FLASH_ALG_UNINITED = 5,
    };

    enum init_mode : uint8_t
    {
        ERASE = 1,
        PROGRAM = 2,
        VERIFY = 3,
    };
}


class swd_prog
{
public:
    static swd_prog& instance()
    {
        static swd_prog instance;
        return instance;
    }

    swd_prog(swd_prog const &) = delete;
    void operator=(swd_prog const &) = delete;

private:
    swd_def::state state = swd_def::UNKNOWN;
    program_syscall_t syscall = {};
    uint32_t code_start = 0;
    uint32_t stack_offset = 0;
    uint32_t func_offset = 0;
    uint32_t ram_addr = 0;
    uint32_t stack_size = 0;
    config_manager *fw_mgr = nullptr;
    led_ctrl &led = led_ctrl::instance();

    static const uint32_t header_blob[];

private:
    swd_prog() = default;
    esp_err_t load_flash_algorithm();
    esp_err_t run_algo_init(swd_def::init_mode mode);
    esp_err_t run_algo_uninit(swd_def::init_mode mode);

public:
    esp_err_t init(config_manager *algo, uint32_t ram_addr = 0x20000000, uint32_t stack_size_byte = 0x200);
    esp_err_t erase_chip();
    esp_err_t erase_sector(uint32_t start_addr, uint32_t end_addr);
    esp_err_t program_page(const uint8_t *buf, size_t len, uint32_t start_addr = UINT32_MAX);
    esp_err_t program_file(const char *path, uint32_t *len_written = nullptr, uint32_t start_addr = UINT32_MAX);
    esp_err_t verify(uint32_t expected_crc, uint32_t start_addr = UINT32_MAX, size_t len = 0);
    esp_err_t self_test(uint16_t test_id, uint8_t *readout_buf, size_t readout_buf_len = 0, uint32_t *func_return_val = nullptr);
};
