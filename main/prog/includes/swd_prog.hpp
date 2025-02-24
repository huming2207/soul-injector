#pragma once

#include <esp_err.h>
#include <swd_host.h>
#include <led_ctrl.hpp>
#include "fw_asset_manager.hpp"

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
    static swd_prog *instance()
    {
        static swd_prog _instance;
        return &_instance;
    }

    swd_prog(swd_prog const &) = delete;
    void operator=(swd_prog const &) = delete;

private:
    swd_def::state state = swd_def::UNKNOWN;
    program_syscall_t syscall = {};
    uint32_t code_start = 0;
    uint32_t code_end = 0;
    uint32_t stack_bottom = 0; // Offset of stack bottom
    uint32_t stack_top = 0; // Offset of stack top
    uint32_t stack_canary = 0; // Random 32-bit word generated on every init
    uint32_t ram_start_addr = 0;
    uint32_t ram_size = 0;
    uint32_t stack_size = 0;
    size_t algo_bin_len = 0;
    led_ctrl &led = led_ctrl::instance();
    uint8_t programmed_hash[32] = {}; // SHA256 for the stuff programmed?

private:
    static const constexpr uint32_t halt_header = 0xBE00BE00; // Two breakpoint instructions. See https://github.com/probe-rs/probe-rs/pull/2883

private:
    swd_prog() = default;
    esp_err_t load_flash_algorithm();
    esp_err_t run_algo_init(swd_def::init_mode mode);
    esp_err_t run_algo_uninit(swd_def::init_mode mode);
    static inline uint32_t next_multiple_of(uint32_t input, uint32_t of);
    inline esp_err_t perform_double_buffered_program(FILE *file, uint32_t len, uint32_t page_size, uint32_t pc_program_page, uint32_t addr_offset);
    inline esp_err_t perform_simple_program(FILE *file, uint32_t len, uint32_t page_size, uint32_t pc_program_page, uint32_t addr_offset);

public:
    esp_err_t init(uint32_t _stack_size = 0x2000);
    esp_err_t erase_chip();
    esp_err_t erase_sector(uint32_t start_addr, uint32_t end_addr);
    esp_err_t program_page(const uint8_t *buf, size_t len, uint32_t start_addr = UINT32_MAX);
    esp_err_t program_file(const char *path, uint32_t *len_written = nullptr, uint32_t start_addr = UINT32_MAX);
    esp_err_t verify(const char *path, uint32_t start_addr = UINT32_MAX, size_t len = 0);
    esp_err_t self_test(uint16_t test_id, uint8_t *readout_buf, size_t readout_buf_len = 0, uint32_t *func_return_val = nullptr);
    static void trigger_nrst();
};
