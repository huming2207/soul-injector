#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <swd_host.h>
#include <esp_log.h>
#include <esp_timer.h>
#include <cstring>
#include <esp_crc.h>
#include <algorithm>
#include "swd_prog.hpp"

#define TAG "swd_prog"

const uint32_t swd_prog::header_blob[] = {
        0xE00ABE00,
        0x062D780D,
        0x24084068,
        0xD3000040,
        0x1E644058,
        0x1C49D1FA,
        0x2A001E52,
        0x04770D1F,
};

esp_err_t swd_prog::load_flash_algorithm()
{
    auto ret = swd_halt_target();
    if (ret < 1) {
        ESP_LOGE(TAG, "Failed when halting");
        state = swd_def::UNKNOWN;
        return ESP_ERR_INVALID_STATE;
    }

    ret = swd_wait_until_halted();
    if (ret < 1) {
        ESP_LOGE(TAG, "Timeout when halting");
        state = swd_def::UNKNOWN;
        return ESP_ERR_INVALID_STATE;
    }

    // Mem structure: 512 bytes stack + flash algorithm binary + buffer
    ret = swd_write_memory(code_start, (uint8_t *)header_blob, sizeof(header_blob));
    if (ret < 1) {
        ESP_LOGE(TAG, "Failed when writing flash algorithm header");
        state = swd_def::UNKNOWN;
        return ESP_FAIL;
    }

    size_t algo_bin_len = 0;
    if (fw_mgr->get_algo_bin_len(&algo_bin_len) != ESP_OK) {
        ESP_LOGE(TAG, "Failed to read algo bin len");
        return ESP_ERR_INVALID_STATE;
    }

    if (heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL) < algo_bin_len) {
        ESP_LOGE(TAG, "Flash algo is too huge");
        return ESP_ERR_NO_MEM;
    }

    auto *algo_bin = static_cast<uint8_t *>(heap_caps_malloc(algo_bin_len, MALLOC_CAP_INTERNAL));
    if (algo_bin == nullptr) {
        ESP_LOGE(TAG, "Failed to allocate flash algo bin buffer");
        return ESP_ERR_NO_MEM;
    }

    if (fw_mgr->get_algo_bin(algo_bin, algo_bin_len) != ESP_OK) {
        ESP_LOGE(TAG, "Failed to read algo bin");
        free(algo_bin);
        return ESP_ERR_INVALID_STATE;
    }

    ret = swd_write_memory(code_start + sizeof(header_blob), algo_bin, algo_bin_len);
    if (ret < 1) {
        ESP_LOGE(TAG, "Failed when writing main flash algorithm");
        state = swd_def::UNKNOWN;

        free(algo_bin);
        return ESP_FAIL;
    }

    state = swd_def::FLASH_ALG_LOADED;
    free(algo_bin);
    return ESP_OK;
}

esp_err_t swd_prog::run_algo_init(swd_def::init_mode mode)
{
    ESP_LOGI(TAG, "Running init, load_addr: 0x%lx, stack_ptr: 0x%lx, static_base: 0x%lx", syscall.breakpoint, syscall.stack_pointer, syscall.static_base);
    uint32_t retry_cnt = 3;
    while (retry_cnt > 0) {
        if (load_flash_algorithm() != ESP_OK) {
            ESP_LOGE(TAG, "Failed when loading flash algorithm");
            return ESP_FAIL;
        }

        auto ret = swd_halt_target();
        if (ret < 1) {
            ESP_LOGE(TAG, "Failed when halting");
            state = swd_def::UNKNOWN;
            return ESP_ERR_INVALID_STATE;
        }

        uint32_t pc_init = 0;
        if (fw_mgr->get_pc_init(&pc_init) != ESP_OK) {
            ESP_LOGE(TAG, "Init func pointer config not found");
            return ESP_ERR_INVALID_STATE;
        }

        uint32_t flash_start_addr = 0;
        if (fw_mgr->get_flash_start_addr(&flash_start_addr) != ESP_OK) {
            ESP_LOGE(TAG, "Flash start addr config not found");
            return ESP_ERR_INVALID_STATE;
        }

        ret = swd_wait_until_halted();
        if (ret < 1) {
            ESP_LOGE(TAG, "Timeout when halting");
            state = swd_def::UNKNOWN;
            return ESP_ERR_INVALID_STATE;
        }

        ESP_LOGI(TAG, "Flash start addr = 0x%lx, pc_init = 0x%lx", flash_start_addr, func_offset + pc_init);

        ret = swd_flash_syscall_exec(
                &syscall,
                func_offset + pc_init, // Init PC (usually) = 1, +0x20 for header (but somehow actually 0?)
                flash_start_addr, // r0 = flash base addr
                0, // r1 = ignored
                mode, 0, // r2 = mode, r3 ignored
                FLASHALGO_RETURN_BOOL,
                nullptr
        );

        if (ret < 1) {
            ESP_LOGW(TAG, "Failed when init algorithm, returned %d, retrying...", ret);
            init(fw_mgr, ram_addr, stack_size); // Re-init SWD as well (so that target will reset)
            retry_cnt -= 1;
        } else {
            state = swd_def::FLASH_ALG_INITED;
            return ESP_OK;
        }
    }

    state = swd_def::UNKNOWN;
    return ESP_FAIL;
}

esp_err_t swd_prog::run_algo_uninit(swd_def::init_mode mode)
{
    auto ret = swd_halt_target();
    if (ret < 1) {
        ESP_LOGE(TAG, "Failed when halting");
        state = swd_def::UNKNOWN;
        return ESP_ERR_INVALID_STATE;
    }

    uint32_t pc_uninit = 0;
    if (fw_mgr->get_pc_uninit(&pc_uninit) != ESP_OK) {
        ESP_LOGE(TAG, "UnInit func pointer config not found");
        return ESP_ERR_INVALID_STATE;
    }

    ret = swd_wait_until_halted();
    if (ret < 1) {
        ESP_LOGE(TAG, "Timeout when halting");
        state = swd_def::UNKNOWN;
        return ESP_ERR_INVALID_STATE;
    }

    ret = swd_flash_syscall_exec(
            &syscall,
            func_offset + pc_uninit, // UnInit PC = 61
            mode,
            0, 0, 0, // r2, r3 = ignored
            FLASHALGO_RETURN_BOOL,
            nullptr
    );

    if (ret < 1) {
        ESP_LOGE(TAG, "Failed when uninit algorithm");
        state = swd_def::UNKNOWN;
        return ESP_FAIL;
    }

    state = swd_def::FLASH_ALG_UNINITED;
    return ESP_OK;
}

esp_err_t swd_prog::init(local_mission_manager *_algo, uint32_t _ram_addr, uint32_t _stack_size_byte)
{
    if (_algo == nullptr) {
        ESP_LOGE(TAG, "Flash algorithm container pointer is null");
        return ESP_ERR_INVALID_ARG;
    }

    if (_stack_size_byte == 0) {
        ESP_LOGE(TAG, "Stack size too small");
        return ESP_ERR_INVALID_ARG;
    }

    fw_mgr = _algo;
    ram_addr = _ram_addr;
    stack_size = _stack_size_byte;

    ESP_LOGI(TAG, "Init target");
    auto ret = swd_init_debug();
    if (ret < 1) {
        ESP_LOGE(TAG, "Failed when init");
        state = swd_def::UNKNOWN;
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "Halt target");
    ret = swd_halt_target();
    if (ret < 1) {
        ESP_LOGE(TAG, "Failed when halting");
        state = swd_def::UNKNOWN;
        return ESP_ERR_INVALID_STATE;
    }

    ret = swd_wait_until_halted();
    if (ret < 1) {
        ESP_LOGE(TAG, "Timeout when halting");
        state = swd_def::UNKNOWN;
        return ESP_ERR_INVALID_STATE;
    }

    // We are using probe-rs style flash algorithm
    uint32_t offset = 0;
    offset += sizeof(header_blob);

    code_start = ram_addr;

    size_t algo_bin_len = 0;
    if (fw_mgr->get_algo_bin_len(&algo_bin_len) != ESP_OK) {
        ESP_LOGE(TAG, "Failed to read algo bin len");
        return ESP_ERR_INVALID_STATE;
    }

    offset += algo_bin_len;
    stack_offset = ram_addr + offset + stack_size + sizeof(header_blob);

    uint32_t data_section_offset = 0;
    if (fw_mgr->get_data_section_offset(&data_section_offset) != ESP_OK) {
        ESP_LOGE(TAG, "Failed to read data section offset");
        return ESP_ERR_INVALID_STATE;
    }

    syscall.breakpoint = code_start + 1; // This is ARM
    syscall.static_base = data_section_offset + sizeof(header_blob); // Length = 512, header = 32 bytes
    syscall.stack_pointer = stack_offset;

    func_offset = ram_addr + sizeof(header_blob);

    ESP_LOGI(TAG, "Addr: code_start: 0x%08lx; stack_offset: 0x%08lx; data_section: 0x%08lx", code_start, stack_offset, data_section_offset);
    ESP_LOGI(TAG, "Addr: stack: 0x%08lx; bkpt: 0x%08lx; func_offset: 0x%08lx", stack_offset, syscall.breakpoint, func_offset);

    state = swd_def::INITIALISED;
    return ESP_OK;
}

esp_err_t swd_prog::erase_chip()
{
    uint32_t pc_erase_all = 0;
    auto nvs_ret = fw_mgr->get_pc_erase_all(&pc_erase_all);
    if (nvs_ret != ESP_OK || pc_erase_all == 0 || pc_erase_all == UINT32_MAX) {
        ESP_LOGE(TAG, "This algorithm doesn't support EraseChip");
        return ESP_ERR_NOT_SUPPORTED;
    }

    ESP_LOGI(TAG, "Running chip erase, pc_erase_all = 0x%08lx", pc_erase_all);

    if (state != swd_def::FLASH_ALG_INITED) {
        ESP_LOGW(TAG, "Flash alg not initialised, doing now");
        auto ret = run_algo_init(swd_def::ERASE);
        if (ret != ESP_OK) return ret;
    }

    auto swd_ret = swd_halt_target();
    if (swd_ret < 1) {
        ESP_LOGE(TAG, "Failed when halting");
        state = swd_def::UNKNOWN;
        return ESP_ERR_INVALID_STATE;
    }

    swd_ret = swd_wait_until_halted();
    if (swd_ret < 1) {
        ESP_LOGE(TAG, "Timeout when halting");
        state = swd_def::UNKNOWN;
        return ESP_ERR_INVALID_STATE;
    }

    led.set_color(0, 0, 60, 1);

    swd_ret = swd_flash_syscall_exec(
            &syscall,
            func_offset + pc_erase_all,
            0, // No arguments
            0, 0, 0, // r1, r2 = ignored
            FLASHALGO_RETURN_BOOL,
            nullptr
    );

    if (swd_ret < 1) {
        ESP_LOGE(TAG, "Chip erase failed, fallback to sector erase");

        auto ret = run_algo_uninit(swd_def::ERASE);
        if (ret != ESP_OK) return ret;

        return ESP_FAIL;
    }

    // Maybe no need to uninit here??
    auto ret = run_algo_uninit(swd_def::ERASE);
    if (ret != ESP_OK) return ret;

    state = swd_def::FLASH_ALG_UNINITED;
    return ret;
}

esp_err_t swd_prog::self_test(uint16_t test_id, uint8_t *readout_buf, size_t readout_buf_len, uint32_t *func_return_val)
{
    uint32_t pc_verify = 0;
    auto nvs_ret = fw_mgr->get_pc_verify(&pc_verify);

    if (nvs_ret != ESP_OK) {
        ESP_LOGE(TAG, "Missing config for Verify/SelfTest");
        return ESP_ERR_NOT_SUPPORTED;
    }

    if (state != swd_def::FLASH_ALG_INITED) {
        ESP_LOGW(TAG, "Flash alg not initialised, doing now");
        auto ret = run_algo_init(swd_def::ERASE);
        if (ret != ESP_OK) return ret;
    }

    auto swd_ret = swd_halt_target();
    if (swd_ret < 1) {
        ESP_LOGE(TAG, "Failed when halting");
        state = swd_def::UNKNOWN;
        return ESP_ERR_INVALID_STATE;
    }

    swd_ret = swd_wait_until_halted();
    if (swd_ret < 1) {
        ESP_LOGE(TAG, "Timeout when halting");
        state = swd_def::UNKNOWN;
        return ESP_ERR_INVALID_STATE;
    }

    swd_ret = swd_flash_syscall_exec(
            &syscall,
            func_offset + pc_verify,
            test_id + 0xfffff000, // r0 is addr, for SI's algo executing 0xfffff000+ to trigger self test
            readout_buf_len, // r1 indicates self test result RAM buffer size (or 0 if not used)
            0, // r2 indicates self test result RAM buffer pointer (or 0, aka. null, if not used) - TODO: need to implement readout buffer copy
            0, // r3 unused
            FLASHALGO_RETURN_VALUE,
            func_return_val
    );

    if (swd_ret < 1) {
        ESP_LOGE(TAG, "Self-test function returned an unknown error");
        return ESP_FAIL;
    }

    return 0;
}

esp_err_t swd_prog::erase_sector(uint32_t start_addr, uint32_t end_addr)
{
    uint32_t flash_sector_size = 0, pc_erase_sector = 0, flash_start_addr = 0;
    auto nvs_ret = fw_mgr->get_sector_size(&flash_sector_size);
    nvs_ret = nvs_ret ?: fw_mgr->get_pc_erase_sector(&pc_erase_sector);
    nvs_ret = nvs_ret ?: fw_mgr->get_flash_start_addr(&flash_start_addr);

    if (nvs_ret != ESP_OK) {
        ESP_LOGE(TAG, "Missing config for EraseSector");
        return ESP_ERR_INVALID_STATE;
    }

    uint32_t sector_cnt = (end_addr - start_addr) / flash_sector_size;
    ESP_LOGI(TAG, "End addr 0x%lx, start addr 0x%lx, sector size %lu", end_addr, start_addr, flash_sector_size);
    if ((end_addr - start_addr) % flash_sector_size != 0 || sector_cnt < 1) {
        ESP_LOGE(TAG, "Misaligned sector address");
        return ESP_ERR_INVALID_ARG;
    }


    if (state != swd_def::FLASH_ALG_INITED) {
        ESP_LOGW(TAG, "Flash alg not initialised, doing now");
        auto ret = run_algo_init(swd_def::ERASE);
        if (ret != ESP_OK) return ret;
    }

    auto swd_ret = swd_halt_target();
    if (swd_ret < 1) {
        ESP_LOGE(TAG, "Failed when halting");
        state = swd_def::UNKNOWN;
        return ESP_ERR_INVALID_STATE;
    }

    swd_ret = swd_wait_until_halted();
    if (swd_ret < 1) {
        ESP_LOGE(TAG, "Timeout when halting");
        state = swd_def::UNKNOWN;
        return ESP_ERR_INVALID_STATE;
    }

    for (uint32_t idx = 0; idx < sector_cnt - 1; idx += 1) {
        swd_ret = swd_flash_syscall_exec(
                &syscall,
                func_offset + pc_erase_sector, // ErasePage PC = 173
                flash_start_addr + (idx * flash_sector_size), // r0 = flash base addr
                0, 0, 0, // r1, r2 = ignored
                FLASHALGO_RETURN_BOOL,
                nullptr
        );

        if (swd_ret < 1) {
            ESP_LOGE(TAG, "Erase function returned an unknown error");
            return ESP_FAIL;
        }

        if(idx % 10 == 0) {
            led.set_color(0, 0, 60, 50);
        } else {
            led.set_color(0, 0, 0, 50);
        }
    }

    auto ret = run_algo_uninit(swd_def::ERASE);
    if (ret != ESP_OK) return ret;

    state = swd_def::FLASH_ALG_UNINITED;
    return ret;
}

esp_err_t swd_prog::program_page(const uint8_t *buf, size_t len, uint32_t start_addr)
{
    if (len % 4 != 0) {
        ESP_LOGE(TAG, "Length is not 32-bit word aligned");
        return ESP_ERR_INVALID_ARG;
    }

    if (state != swd_def::FLASH_ALG_INITED) {
        auto ret = run_algo_init(swd_def::PROGRAM);
        if (ret != ESP_OK) return ret;
    }

    auto swd_ret = swd_halt_target();
    if (swd_ret < 1) {
        ESP_LOGE(TAG, "Failed when halting");
        state = swd_def::UNKNOWN;
        return ESP_ERR_INVALID_STATE;
    }

    uint32_t page_size = 0, pc_program_page = 0, flash_start_addr = 0;
    auto nvs_ret = fw_mgr->get_page_size(&page_size);
    nvs_ret = nvs_ret ?: fw_mgr->get_pc_program_page(&pc_program_page);
    nvs_ret = nvs_ret ?: fw_mgr->get_flash_start_addr(&flash_start_addr);

    if (nvs_ret != ESP_OK) {
        ESP_LOGE(TAG, "Missing config for ProgramPage");
        return ESP_ERR_INVALID_STATE;
    }

    swd_ret = swd_wait_until_halted();
    if (swd_ret < 1) {
        ESP_LOGE(TAG, "Timeout when halting");
        state = swd_def::UNKNOWN;
        return ESP_ERR_INVALID_STATE;
    }


    ESP_LOGI(TAG, "program_page: page_size: %lu, pc_prg_page=0x%lx, flash_start_addr=0x%lx", page_size, pc_program_page, flash_start_addr);

    uint32_t addr_offset = flash_start_addr + (start_addr == UINT32_MAX ? 0 : start_addr);
    uint32_t remain_len = len;
    for (uint32_t page_idx = 0; page_idx < (len / page_size); page_idx += 1) {
        uint32_t write_size = std::min(page_size, remain_len);
        ESP_LOGD(TAG, "program_page: write size: %lu", write_size);
        swd_ret = swd_write_memory(syscall.static_base, (uint8_t *)(buf + (page_idx * page_size)), write_size);
        if (swd_ret < 1) {
            ESP_LOGE(TAG, "Failed when writing RAM cache");
            state = swd_def::UNKNOWN;
            return ESP_ERR_INVALID_STATE;
        }

        ESP_LOGD(TAG, "Writing page 0x%lx, size %lu", 0x08000000 + (page_idx * page_size), write_size);
        swd_ret = swd_flash_syscall_exec(
                &syscall,
                func_offset + pc_program_page, // ErasePage PC = 305
                addr_offset + (page_idx * page_size), // r0 = flash base addr
                write_size,
                syscall.static_base, 0, // r1 = len, r2 = buf addr
                FLASHALGO_RETURN_BOOL,
                nullptr
        );

        if(page_idx % 2 == 0) {
            led.set_color(50, 50, 0, 20);
        } else {
            led.set_color(0, 0, 0, 20);
        }

        remain_len -= write_size;
    }

    if (swd_ret < 1) {
        ESP_LOGE(TAG, "Program function returned an unknown error");
        state = swd_def::UNKNOWN;
        return ESP_ERR_INVALID_STATE;
    }

    auto ret = run_algo_uninit(swd_def::PROGRAM);
    if (ret != ESP_OK) return ret;

    state = swd_def::FLASH_ALG_UNINITED;
    return ret;
}

esp_err_t swd_prog::program_file(const char *path, uint32_t *len_written, uint32_t start_addr)
{
    if (path == nullptr) {
        ESP_LOGE(TAG, "Path is null!");
        return ESP_ERR_INVALID_ARG;
    }

    FILE *file = fopen(path, "rb");
    if (file == nullptr) {
        ESP_LOGE(TAG, "Failed when reading firmware file");
        return ESP_ERR_NOT_FOUND;
    }

    fseek(file, 0, SEEK_END);
    size_t len = ftell(file);
    if (len < 4 || len % 4 != 0) {
        ESP_LOGE(TAG, "Manifest in a wrong length: %u", len);
        fclose(file);
        return ESP_ERR_INVALID_SIZE;
    }

    if (len_written != nullptr) {
        *len_written = len;
    }

    fseek(file, 0, SEEK_SET);

    if (state != swd_def::FLASH_ALG_INITED) {
        auto ret = run_algo_init(swd_def::PROGRAM);
        if (ret != ESP_OK) return ret;
    }

    auto swd_ret = swd_halt_target();
    if (swd_ret < 1) {
        ESP_LOGE(TAG, "Failed when halting");
        state = swd_def::UNKNOWN;
        fclose(file);
        return ESP_ERR_INVALID_STATE;
    }

    uint32_t page_size = 0, pc_program_page = 0, flash_start_addr = 0;
    auto nvs_ret = fw_mgr->get_page_size(&page_size);
    nvs_ret = nvs_ret ?: fw_mgr->get_pc_program_page(&pc_program_page);
    nvs_ret = nvs_ret ?: fw_mgr->get_flash_start_addr(&flash_start_addr);

    if (nvs_ret != ESP_OK) {
        ESP_LOGE(TAG, "Missing config for ProgramPage");
        return ESP_ERR_NOT_FOUND;
    }

    swd_ret = swd_wait_until_halted();
    if (swd_ret < 1) {
        ESP_LOGE(TAG, "Timeout when halting");
        state = swd_def::UNKNOWN;
        fclose(file);
        return ESP_ERR_TIMEOUT;
    }

    uint32_t addr_offset = flash_start_addr + (start_addr == UINT32_MAX ? 0 : start_addr);
    uint32_t remain_len = len;
    auto *buf = new uint8_t[page_size];
    memset(buf, 0, page_size);

    ESP_LOGI(TAG, "program_file: page_size: %lu, pc_prg_page=0x%lx, flash_start_addr=0x%lx", page_size, pc_program_page, flash_start_addr);

    for (uint32_t page_idx = 0; page_idx < ((len / page_size) + ((len % page_size != 0) ? 1 : 0)); page_idx += 1) {
        uint32_t write_size = std::min(page_size, remain_len);
        size_t read_len = fread(buf, 1, write_size, file);
        ESP_LOGD(TAG, "program_file: write size: %lu", write_size);
        if (read_len != write_size) {
            ESP_LOGW(TAG, "Trying to read %lu bytes but got only %u bytes", write_size, read_len);
            write_size = read_len;
        }

        swd_ret = swd_write_memory(syscall.static_base + stack_size, (uint8_t *)buf, write_size); // TODO: temp fix
        if (swd_ret < 1) {
            ESP_LOGE(TAG, "Failed when writing RAM cache");
            delete[] buf;
            state = swd_def::UNKNOWN;
            return ESP_ERR_INVALID_STATE;
        }

        ESP_LOGD(TAG, "Writing page 0x%lx, size %lu from RAM 0x%lx", addr_offset + (page_idx * page_size), write_size, syscall.static_base + stack_size);
        swd_ret = swd_flash_syscall_exec(
                &syscall,
                func_offset + pc_program_page, // ErasePage PC = 305
                addr_offset + (page_idx * page_size), // r0 = flash base addr
                write_size,
                syscall.static_base + stack_size, 0, // r1 = len, r2 = buf addr
                FLASHALGO_RETURN_BOOL,
                nullptr
        );

        if(page_idx % 2 == 0) {
            led.set_color(50, 50, 0, 20);
        } else {
            led.set_color(0, 0, 0, 20);
        }

        remain_len -= write_size;
    }

    delete[] buf;

    if (swd_ret < 1) {
        ESP_LOGE(TAG, "Program function returned an unknown error");
        state = swd_def::UNKNOWN;
        return ESP_ERR_INVALID_STATE;
    }

    fclose(file);
    auto ret = run_algo_uninit(swd_def::PROGRAM);
    if (ret != ESP_OK) return ret;

    state = swd_def::FLASH_ALG_UNINITED;
    return ret;
}

esp_err_t swd_prog::verify(uint32_t expected_crc, uint32_t start_addr, size_t len)
{
    auto swd_ret = swd_halt_target();
    if (swd_ret < 1) {
        ESP_LOGE(TAG, "Failed when halting");
        state = swd_def::UNKNOWN;
        return ESP_ERR_INVALID_STATE;
    }

    uint32_t flash_start_addr = 0, flash_end_addr = 0;
    auto nvs_ret = fw_mgr->get_flash_end_addr(&flash_end_addr);
    nvs_ret = nvs_ret ?: fw_mgr->get_flash_start_addr(&flash_start_addr);

    if (nvs_ret != ESP_OK) {
        ESP_LOGE(TAG, "Missing config for verify");
        return ESP_ERR_INVALID_STATE;
    }

    uint32_t actual_read_addr = (start_addr == UINT32_MAX) ? flash_start_addr : start_addr;
    uint32_t actual_len = (len == 0) ? (flash_end_addr - flash_start_addr) : len;
    uint32_t actual_crc = 0;
    size_t remain_len = actual_len;
    uint32_t offset = 0;

    while(remain_len > 0) {
        uint8_t buf[1024] = { 0 };
        uint32_t read_len = std::min(sizeof(buf), remain_len);
        swd_ret = swd_read_memory((actual_read_addr + offset), buf, read_len);
        if (swd_ret < 1) {
            ESP_LOGE(TAG, "Failed when reading flash");
            return ESP_ERR_INVALID_STATE;
        }

        actual_crc = esp_crc32_le(actual_crc, buf, read_len);
        offset += read_len;
        remain_len -= read_len;
    }

    if (expected_crc != actual_crc) {
        ESP_LOGE(TAG, "CRC mismatched, expected 0x%lx, actual 0x%lx", expected_crc, actual_crc);
        return ESP_ERR_INVALID_CRC;
    } else {
        ESP_LOGI(TAG, "CRC matched, expected 0x%lx, actual 0x%lx", expected_crc, actual_crc);
    }

    return ESP_OK;
}

void swd_prog::trigger_nrst()
{
    swd_trigger_nrst();
}

