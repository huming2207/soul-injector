#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include <swd_host.h>
#include <esp_log.h>
#include <esp_timer.h>

#include <cstring>
#include <algorithm>
#include <esp_random.h>

#include "fw_asset_manager.hpp"
#include "swd_prog.hpp"

#define TAG "swd_prog"

const si::config::flash_algorithm &swd_prog::algo()
{
    return fw_asset_manager::instance()->config().algo;
}

esp_err_t swd_prog::load_flash_algorithm()
{
    swd_off();
    vTaskDelay(1);
    swd_init();
    vTaskDelay(1);
    swd_trigger_nrst();
    vTaskDelay(1);

    auto ret = swd_init_debug();
    if (ret < 1) {
        ESP_LOGE(TAG, "load_algo: SWD init failed");
        state = swd_def::UNKNOWN;
        return ESP_ERR_INVALID_STATE;
    }

    vTaskDelay(1);

    ret = swd_halt_target();
    if (ret < 1) {
        ESP_LOGE(TAG, "load_algo: Failed when halting");
        state = swd_def::UNKNOWN;
        return ESP_ERR_INVALID_STATE;
    }

    ret = swd_wait_until_halted();
    if (ret < 1) {
        ESP_LOGE(TAG, "load_algo: Timeout when halting");
        state = swd_def::UNKNOWN;
        return ESP_ERR_INVALID_STATE;
    }

    const si::config::flash_algorithm &fa = algo();
    algo_bin_len = fa.algo_bin_len;
    code_start = fa.load_address;

    // The decoded algorithm blob is already arena-backed (PSRAM); no copy.
    ESP_LOGI(TAG, "load_algo: algo '%s', %zu bytes @ 0x%08lx", fa.name, algo_bin_len, code_start);

    code_end = next_multiple_of((code_start + algo_bin_len),
                                8); // Force align to 8, for 32-bit ARM Cortex-M. I don't know why but probe-rs did this.

    ret = swd_write_word(code_start - sizeof(uint32_t), halt_header);
    if (ret < 1) {
        ESP_LOGE(TAG, "load_algo: Failed when writing flash algorithm header");
        state = swd_def::UNKNOWN;
        return ESP_FAIL;
    }

    ret = swd_write_memory(code_start, const_cast<uint8_t *>(fa.algo_bin), algo_bin_len);
    if (ret < 1) {
        ESP_LOGE(TAG, "load_algo: Failed when writing main flash algorithm");
        state = swd_def::UNKNOWN;
        return ESP_FAIL;
    }

    // Write stack canary here so it survives target resets between algorithm loads
    ret = swd_write_word(stack_bottom, stack_canary);
    if (ret < 1) {
        ESP_LOGE(TAG, "load_algo: Failed when writing stack canary");
        state = swd_def::UNKNOWN;
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "load_algo: OK");
    state = swd_def::FLASH_ALG_LOADED;
    return ESP_OK;
}

esp_err_t swd_prog::run_algo_init(swd_def::init_mode mode)
{
    const si::config::flash_algorithm &fa = algo();

    ESP_LOGI(TAG, "algo_init: Running init, load_addr: 0x%lx, stack_ptr: 0x%lx, static_base: 0x%lx", syscall.breakpoint,
             syscall.stack_pointer, syscall.static_base);
    uint32_t retry_cnt = 3;
    while (retry_cnt > 0) {
        if (load_flash_algorithm() != ESP_OK) {
            ESP_LOGE(TAG, "algo_init: Failed when loading flash algorithm");
            return ESP_FAIL;
        }

        if (!fa.pc_init.has_value()) {
            ESP_LOGE(TAG, "algo_init: Init function (pc_init) not configured");
            return ESP_ERR_NOT_FOUND;
        }

        if (!fa.flash_start.has_value()) {
            ESP_LOGE(TAG, "algo_init: Flash start address not configured");
            return ESP_ERR_NOT_FOUND;
        }

        auto ret = swd_wait_until_halted();
        if (ret < 1) {
            ESP_LOGE(TAG, "algo_init: Timeout when halting");
            state = swd_def::UNKNOWN;
            return ESP_ERR_INVALID_STATE;
        }

        ESP_LOGD(TAG, "algo_init: Flash start addr = 0x%lx, pc_init = 0x%lx", fa.flash_start.value(), fa.pc_init.value());

        ret = swd_flash_syscall_exec(&syscall,
                                     fa.pc_init.value(),     // Init PC (usually) = 1, +0x20 for header (but somehow actually 0?)
                                     fa.flash_start.value(), // r0 = flash base addr
                                     0,                      // r1 = ignored
                                     mode, 0,                // r2 = mode, r3 ignored
                                     FLASHALGO_RETURN_BOOL, nullptr);

        if (ret < 1) {
            ESP_LOGW(TAG, "algo_init: Failed when init algorithm, returned %d, retrying...", ret);
            init(stack_size); // Re-init SWD as well (so that target will reset)
            retry_cnt -= 1;
        } else {
            ESP_LOGI(TAG, "algo_init: Init() OK");
            state = swd_def::FLASH_ALG_INITED;
            return ESP_OK;
        }
    }

    state = swd_def::UNKNOWN;
    return ESP_FAIL;
}

esp_err_t swd_prog::run_algo_uninit(swd_def::init_mode mode)
{
    const si::config::flash_algorithm &fa = algo();

    auto ret = swd_halt_target();
    if (ret < 1) {
        ESP_LOGE(TAG, "Failed when halting");
        state = swd_def::UNKNOWN;
        return ESP_ERR_INVALID_STATE;
    }

    if (!fa.pc_uninit.has_value()) {
        ESP_LOGE(TAG, "UnInit function (pc_uninit) not configured");
        return ESP_ERR_NOT_FOUND;
    }

    ret = swd_wait_until_halted();
    if (ret < 1) {
        ESP_LOGE(TAG, "Timeout when halting");
        state = swd_def::UNKNOWN;
        return ESP_ERR_INVALID_STATE;
    }

    ESP_LOGI(TAG, "Calling uninit, pc=0x%08lx", fa.pc_uninit.value());
    ret = swd_flash_syscall_exec(&syscall, fa.pc_uninit.value(), // UnInit PC = 61
                                 mode, 0, 0, 0,                  // r2, r3 = ignored
                                 FLASHALGO_RETURN_BOOL, nullptr);

    if (ret < 1) {
        ESP_LOGE(TAG, "Failed when uninit algorithm");
        state = swd_def::UNKNOWN;
        return ESP_FAIL;
    }

    // Check stack canary
    uint32_t curr_stack_canary = 0;
    ret = swd_read_word(stack_bottom, &curr_stack_canary);
    if (ret < 1) {
        ESP_LOGE(TAG, "Failed when reading stack canary");
        state = swd_def::UNKNOWN;
        return ESP_FAIL;
    }

    if (curr_stack_canary != stack_canary) {
        ESP_LOGE(TAG, "Possible stack overflow detected! 0x%08lx vs. 0x%08lx", stack_canary, curr_stack_canary);
        state = swd_def::UNKNOWN;
        return ESP_FAIL;
    }

    state = swd_def::FLASH_ALG_UNINITED;
    return ESP_OK;
}

esp_err_t swd_prog::init(uint32_t _stack_size)
{
    if (_stack_size == 0) {
        ESP_LOGE(TAG, "Stack size too small");
        return ESP_ERR_INVALID_ARG;
    }

    const si::config::target_config &cfg = fw_asset_manager::instance()->config();
    const si::config::ram_region *ram = cfg.largest_ram_region();
    if (ram == nullptr || ram->size() == 0) {
        ESP_LOGE(TAG, "No usable RAM region in target.yaml memory_map");
        return ESP_ERR_NOT_FOUND;
    }

    // Use the LARGEST contiguous region, not a fake span across all regions.
    ram_start_addr = ram->start;
    ram_size = ram->size();

    stack_size = _stack_size;

    ESP_LOGI(TAG, "RAM starts 0x%08lx, len %lu", ram_start_addr, ram_size);
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

    if (load_flash_algorithm() != ESP_OK) {
        ESP_LOGE(TAG, "Failed when loading flash algorithm");
        return ESP_FAIL;
    }

    const si::config::flash_algorithm &fa = algo();

    if (!fa.data_section_offset.has_value()) {
        ESP_LOGE(TAG, "data_section_offset not configured");
        return ESP_ERR_NOT_FOUND;
    }

    if (!fa.page_size.has_value()) {
        ESP_LOGE(TAG, "page_size not configured");
        return ESP_ERR_NOT_FOUND;
    }

    // TODO: stack_bottom still may not be correct, it probably should consider the SelfTestInfo section, not just simply add two flash pages.
    stack_bottom =
        next_multiple_of((code_end + fa.page_size.value() * 2),
                         8); // We only consider the RAM is enough to fit contiguous stuff of .text, .bss and .data for now.
    stack_top = stack_bottom + stack_size;
    stack_canary = esp_random();

    ESP_LOGI(TAG, "Stack: size=%lu top=0x%08lx, bottom=0x%08lx, canary=0x%08lx", stack_size, stack_top, stack_bottom,
             stack_canary);

    syscall.breakpoint = code_start - sizeof(uint32_t) +
                         1; // Breakpoint is where the halt word (2x of breakpoint instructions) + 1 byte for thumb mode
    syscall.static_base = fa.data_section_offset.value(); // BSS, also don't forget the header = 32 bytes
    syscall.stack_pointer = stack_top;

    ESP_LOGI(TAG, "Addr: ram_start_addr: 0x%08lx; data_section: 0x%08lx", ram_start_addr, syscall.static_base);
    ESP_LOGI(TAG, "Addr: stack top: 0x%08lx, bottom: 0x%08lx breakpoint: 0x%08lx, static_base: 0x%08lx", stack_top, stack_bottom,
             syscall.breakpoint, syscall.static_base);

    state = swd_def::INITIALISED;
    return ESP_OK;
}

esp_err_t swd_prog::erase_chip()
{
    const si::config::flash_algorithm &fa = algo();

    if (!fa.pc_erase_all.has_value()) {
        ESP_LOGE(TAG, "This algorithm doesn't support EraseChip (pc_erase_all absent)");
        return ESP_ERR_NOT_SUPPORTED;
    }

    ESP_LOGI(TAG, "Running chip erase, pc_erase_all = 0x%08lx", fa.pc_erase_all.value());

    if (state != swd_def::FLASH_ALG_INITED) {
        ESP_LOGW(TAG, "Flash alg not initialised, doing now");
        auto ret = run_algo_init(swd_def::ERASE);
        if (ret != ESP_OK)
            return ret;
    }

    auto swd_ret = swd_halt_target();
    if (swd_ret < 1) {
        ESP_LOGE(TAG, "Failed when init");
        state = swd_def::UNKNOWN;
        return ESP_ERR_INVALID_STATE;
    }

    swd_ret = swd_wait_until_halted();
    if (swd_ret < 1) {
        ESP_LOGE(TAG, "Timeout when halting");
        state = swd_def::UNKNOWN;
        return ESP_ERR_INVALID_STATE;
    }

    led.set_color(0, 0, 60);

    ESP_LOGI(TAG, "Calling chip erase...");

    swd_ret = swd_flash_syscall_exec(&syscall, fa.pc_erase_all.value(), 0, // No arguments
                                     0, 0, 0,                              // r1, r2 = ignored
                                     FLASHALGO_RETURN_BOOL, nullptr);

    if (swd_ret < 1) {
        ESP_LOGE(TAG, "Chip erase failed, fallback to sector erase");

        auto ret = run_algo_uninit(swd_def::ERASE);
        if (ret != ESP_OK)
            return ret;

        return ESP_FAIL;
    } else {
        ESP_LOGI(TAG, "Chip erase OK!");
    }

    // Maybe no need to uninit here??
    auto ret = run_algo_uninit(swd_def::ERASE);
    if (ret != ESP_OK)
        return ret;

    state = swd_def::FLASH_ALG_UNINITED;
    return ret;
}

esp_err_t swd_prog::self_test(uint32_t test_id, uint8_t *readout_buf, size_t readout_buf_len, uint32_t *func_return_val)
{
    const si::config::flash_algorithm &fa = algo();

    if (!fa.pc_verify.has_value()) {
        ESP_LOGE(TAG, "Verify/SelfTest function (pc_verify) not configured");
        return ESP_ERR_NOT_SUPPORTED;
    }

    if (state != swd_def::FLASH_ALG_INITED) {
        ESP_LOGW(TAG, "Flash alg not initialised, doing now");
        auto ret = run_algo_init(swd_def::ERASE);
        if (ret != ESP_OK)
            return ret;
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
        &syscall, fa.pc_verify.value(),
        test_id,         // r0 is addr
        readout_buf_len, // r1 indicates self test result RAM buffer size (or 0 if not used)
        0, // r2 indicates self test result RAM buffer pointer (or 0, aka. null, if not used) - TODO: need to implement readout buffer copy
        0, // r3 unused
        FLASHALGO_RETURN_VALUE, func_return_val);

    if (swd_ret < 1) {
        ESP_LOGE(TAG, "Self-test function returned an unknown error");
        return ESP_FAIL;
    }

    return ESP_OK;
}

esp_err_t swd_prog::erase_sector(uint32_t start_addr, uint32_t end_addr)
{
    const si::config::flash_algorithm &fa = algo();

    // NOTE: sector granularity follows page_size, matching the historical
    // behaviour (get_sector_size used to return page_size).
    if (!fa.page_size.has_value() || !fa.pc_erase_sector.has_value() || !fa.flash_start.has_value()) {
        ESP_LOGE(TAG, "Missing config for EraseSector (page_size/pc_erase_sector/flash start)");
        return ESP_ERR_INVALID_STATE;
    }

    uint32_t flash_sector_size = fa.page_size.value();
    uint32_t sector_cnt = (end_addr - start_addr) / flash_sector_size;
    ESP_LOGI(TAG, "End addr 0x%lx, start addr 0x%lx, sector size %lu", end_addr, start_addr, flash_sector_size);
    if ((end_addr - start_addr) % flash_sector_size != 0 || sector_cnt < 1) {
        ESP_LOGE(TAG, "Misaligned sector address");
        return ESP_ERR_INVALID_ARG;
    }

    if (state != swd_def::FLASH_ALG_INITED) {
        ESP_LOGW(TAG, "Flash alg not initialised, doing now");
        auto ret = run_algo_init(swd_def::ERASE);
        if (ret != ESP_OK)
            return ret;
    }

    auto swd_ret = swd_halt_target();
    if (swd_ret < 1) {
        ESP_LOGE(TAG, "Failed when init");
        state = swd_def::UNKNOWN;
        return ESP_ERR_INVALID_STATE;
    }

    swd_ret = swd_wait_until_halted();
    if (swd_ret < 1) {
        ESP_LOGE(TAG, "Timeout when halting");
        state = swd_def::UNKNOWN;
        return ESP_ERR_INVALID_STATE;
    }

    for (uint32_t idx = 0; idx < sector_cnt; idx += 1) {
        swd_ret = swd_flash_syscall_exec(&syscall, fa.pc_erase_sector.value(),               // ErasePage PC = 173
                                         fa.flash_start.value() + (idx * flash_sector_size), // r0 = flash base addr
                                         0, 0, 0,                                            // r1, r2 = ignored
                                         FLASHALGO_RETURN_BOOL, nullptr);

        if (swd_ret < 1) {
            ESP_LOGE(TAG, "Erase function returned an unknown error");
            return ESP_FAIL;
        }

        if (idx % 10 == 0) {
            led.set_color(0, 0, 60);
        } else {
            led.set_color(0, 0, 0);
        }
    }

    auto ret = run_algo_uninit(swd_def::ERASE);
    if (ret != ESP_OK)
        return ret;

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
        if (ret != ESP_OK)
            return ret;
    }

    auto swd_ret = swd_halt_target();
    if (swd_ret < 1) {
        ESP_LOGE(TAG, "Failed when halting");
        state = swd_def::UNKNOWN;
        return ESP_ERR_INVALID_STATE;
    }

    const si::config::flash_algorithm &fa = algo();
    if (!fa.page_size.has_value() || !fa.pc_program_page.has_value() || !fa.flash_start.has_value()) {
        ESP_LOGE(TAG, "Missing config for ProgramPage");
        return ESP_ERR_INVALID_STATE;
    }

    swd_ret = swd_wait_until_halted();
    if (swd_ret < 1) {
        ESP_LOGE(TAG, "Timeout when halting");
        state = swd_def::UNKNOWN;
        return ESP_ERR_INVALID_STATE;
    }

    ESP_LOGI(TAG, "program_page: page_size: %lu, pc_prg_page=0x%lx, flash_start_addr=0x%lx", fa.page_size.value(),
             fa.pc_program_page.value(), fa.flash_start.value());

    uint32_t addr_offset = fa.flash_start.value() + (start_addr == UINT32_MAX ? 0 : start_addr);
    uint32_t remain_len = len;
    for (uint32_t page_idx = 0; page_idx < (len / fa.page_size.value()); page_idx += 1) {
        uint32_t write_size = std::min(fa.page_size.value(), remain_len);
        ESP_LOGD(TAG, "program_page: write size: %lu", write_size);
        swd_ret = swd_write_memory(stack_top + stack_size, (uint8_t *)(buf + (page_idx * fa.page_size.value())), write_size);
        if (swd_ret < 1) {
            ESP_LOGE(TAG, "Failed when writing RAM cache");
            state = swd_def::UNKNOWN;
            return ESP_ERR_INVALID_STATE;
        }

        ESP_LOGD(TAG, "Writing page 0x%lx, size %lu", 0x08000000 + (page_idx * fa.page_size.value()), write_size);
        swd_ret = swd_flash_syscall_exec(&syscall, fa.pc_program_page.value(),            // ErasePage PC = 305
                                         addr_offset + (page_idx * fa.page_size.value()), // r0 = flash base addr
                                         write_size, stack_top + stack_size, 0,           // r1 = len, r2 = buf addr
                                         FLASHALGO_RETURN_BOOL, nullptr);

        if (page_idx % 2 == 0) {
            led.set_color(50, 50, 0);
        } else {
            led.set_color(0, 0, 0);
        }

        remain_len -= write_size;
    }

    if (swd_ret < 1) {
        ESP_LOGE(TAG, "Program function returned an unknown error");
        state = swd_def::UNKNOWN;
        return ESP_ERR_INVALID_STATE;
    }

    auto ret = run_algo_uninit(swd_def::PROGRAM);
    if (ret != ESP_OK)
        return ret;

    state = swd_def::FLASH_ALG_UNINITED;
    return ret;
}

esp_err_t swd_prog::program_file(const char *path, uint32_t *len_written, uint32_t start_addr)
{
    if (path == nullptr) {
        ESP_LOGE(TAG, "Path is null!");
        return ESP_ERR_INVALID_ARG;
    }

    FILE *file = nullptr;
    int retry = 5;
    while (retry-- > 0) {
        file = fopen(path, "rb");
        if (file != nullptr)
            break;
        ESP_LOGW(TAG, "Failed to open %s, retrying... (%d left)", path, retry);
        vTaskDelay(pdMS_TO_TICKS(200));
    }

    if (file == nullptr) {
        ESP_LOGE(TAG, "Failed when reading firmware file: %s", path);
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
        if (ret != ESP_OK) {
            fclose(file);
            return ret;
        }
    }

    const si::config::flash_algorithm &fa = algo();
    if (!fa.page_size.has_value() || !fa.pc_program_page.has_value() || !fa.flash_start.has_value()) {
        ESP_LOGE(TAG, "Missing config for ProgramPage");
        fclose(file);
        return ESP_ERR_NOT_FOUND;
    }

    uint32_t addr_offset = fa.flash_start.value() + (start_addr == UINT32_MAX ? 0 : start_addr);
    ESP_LOGD(TAG, "program_file: page_size: %lu, pc_prg_page=0x%lx, flash_start_addr=0x%lx", fa.page_size.value(),
             fa.pc_program_page.value(), fa.flash_start.value());

    esp_err_t ret = ESP_OK;
    uint32_t max_possible_buffer_addr = (stack_top + stack_size + fa.page_size.value() * 2);
    int64_t ts = esp_timer_get_time();
    if (ram_start_addr + ram_size >= max_possible_buffer_addr) {
        ret = perform_double_buffered_program(file, len, fa.page_size.value(), fa.pc_program_page.value(), addr_offset);
    } else {
        ret = perform_simple_program(file, len, fa.page_size.value(), fa.pc_program_page.value(), addr_offset);
    }

    ts = esp_timer_get_time() - ts;
    double speed = len / (static_cast<double>(ts) / 1000000.0);
    ESP_LOGI(TAG, "program_file: OK, len=%lu, speed=%.2f bytes/sec", len, speed);

    fclose(file);
    ret = ret ?: run_algo_uninit(swd_def::PROGRAM);
    if (ret != ESP_OK)
        return ret;

    state = swd_def::FLASH_ALG_UNINITED;
    return ret;
}

esp_err_t swd_prog::verify(const char *path, uint32_t start_addr, size_t len)
{
    const si::config::flash_algorithm &fa = algo();

    auto swd_ret = swd_halt_target();
    if (swd_ret < 1) {
        ESP_LOGE(TAG, "Failed when halting");
        state = swd_def::UNKNOWN;
        return ESP_ERR_INVALID_STATE;
    }

    if (!fa.flash_start.has_value()) {
        ESP_LOGE(TAG, "Flash start address not configured (needed for verify)");
        return ESP_ERR_INVALID_STATE;
    }

    uint32_t flash_start_addr = fa.flash_start.value();
    uint32_t actual_read_addr = (start_addr == UINT32_MAX) ? flash_start_addr : start_addr;
    uint32_t actual_len = len;
    uint32_t offset = 0;

    if (actual_len == 0) {
        ESP_LOGE(TAG, "verify: zero length, nothing to verify");
        return ESP_ERR_INVALID_ARG;
    }

    FILE *file = fopen(path, "rb");
    if (file == nullptr) {
        ESP_LOGE(TAG, "Original firmware does not exist");
        return ESP_ERR_NOT_FOUND;
    }

    size_t remain_len = actual_len;
    while (remain_len > 0) {
        uint8_t target_buf[512] = {0};
        uint8_t orig_buf[512] = {0};
        uint32_t read_len = std::min(sizeof(target_buf), remain_len);
        swd_ret = swd_read_memory((actual_read_addr + offset), target_buf, read_len);
        if (swd_ret < 1) {
            ESP_LOGE(TAG, "Failed when reading flash");
            fclose(file);
            return ESP_ERR_INVALID_STATE;
        }

        size_t orig_read_len = fread(orig_buf, 1, read_len, file);
        if (orig_read_len < read_len) {
            ESP_LOGE(TAG, "Read failed, orig read %u expect %lu", orig_read_len, read_len);
            fclose(file);
            return ESP_ERR_INVALID_STATE;
        }

        if (memcmp(orig_buf, target_buf, read_len) != 0) {
            ESP_LOGE(TAG, "Verify failed at addr 0x%08lx, read len %lu", (actual_read_addr + offset), read_len);
            fclose(file);
            return ESP_ERR_INVALID_CRC;
        }

        offset += read_len;
        remain_len -= read_len;
    }

    ESP_LOGI(TAG, "verify: OK");
    fclose(file);
    return ESP_OK;
}

void swd_prog::trigger_nrst()
{
    swd_trigger_nrst();
}

uint32_t swd_prog::next_multiple_of(uint32_t input, uint32_t of)
{
    if (of == 0) {
        return 0;
    }
    return ((input + of - 1) / of) * of;
}

esp_err_t swd_prog::perform_double_buffered_program(FILE *file, uint32_t len, uint32_t page_size, uint32_t pc_program_page,
                                                    uint32_t addr_offset)
{
    uint32_t remain_len = len;
    uint8_t swd_ret = 0;
    uint8_t curr_buf = 0;
    uint32_t curr_buf_addr = stack_top + stack_size;
    auto *buf = (uint8_t *)heap_caps_malloc(page_size, MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA);
    memset(buf, 0, page_size);

    for (uint32_t page_idx = 0; page_idx < ((len / page_size) + ((len % page_size != 0) ? 1 : 0)); page_idx += 1) {
        uint32_t write_size = std::min(page_size, remain_len);
        size_t read_len = fread(buf, 1, write_size, file);
        ESP_LOGD(TAG, "program_file: write size: %lu", write_size);
        if (read_len != write_size) {
            ESP_LOGW(TAG, "Trying to read %lu bytes but got only %u bytes", write_size, read_len);
            write_size = read_len;
        }

        curr_buf_addr = curr_buf == 0 ? (stack_top + stack_size) : (stack_top + stack_size + page_size);
        swd_ret = swd_write_memory(curr_buf_addr, (uint8_t *)buf, write_size);
        if (swd_ret < 1) {
            ESP_LOGE(TAG, "Failed when writing RAM cache");
            free(buf);
            state = swd_def::UNKNOWN;
            return ESP_ERR_INVALID_STATE;
        }

        swd_ret = swd_flash_syscall_wait_result(FLASHALGO_RETURN_BOOL, nullptr);
        if (swd_ret < 1) {
            ESP_LOGE(TAG, "Failed when checking programming state");
            free(buf);
            state = swd_def::UNKNOWN;
            return ESP_ERR_INVALID_STATE;
        }

        ESP_LOGD(TAG, "Writing page 0x%08lx, size %lu from RAM 0x%08lx", addr_offset + (page_idx * page_size), write_size,
                 curr_buf_addr);
        swd_ret = swd_flash_syscall_exec_async(&syscall, pc_program_page,
                                               addr_offset + (page_idx * page_size), // r0 = flash base addr
                                               write_size,                           // r1 = length
                                               curr_buf_addr, 0                      // r2 = buf addr
        );

        if (swd_ret < 1) {
            ESP_LOGE(TAG, "Failed when programming data to target");
            free(buf);
            state = swd_def::UNKNOWN;
            return ESP_ERR_INVALID_STATE;
        }

        curr_buf = (curr_buf == 1) ? 0 : 1; // Rotate buffer

        if (page_idx % 2 == 0) {
            led.set_color(50, 50, 0);
        } else {
            led.set_color(0, 0, 0);
        }

        remain_len -= write_size;
    }

    swd_ret = swd_flash_syscall_wait_result(FLASHALGO_RETURN_BOOL, nullptr);
    if (swd_ret < 1) {
        ESP_LOGE(TAG, "Failed when checking programming state after finish");
        free(buf);
        state = swd_def::UNKNOWN;
        return ESP_ERR_INVALID_STATE;
    }

    free(buf);
    return ESP_OK;
}

esp_err_t swd_prog::perform_simple_program(FILE *file, uint32_t len, uint32_t page_size, uint32_t pc_program_page,
                                           uint32_t addr_offset)
{
    uint32_t remain_len = len;
    uint8_t swd_ret = 0;
    uint32_t curr_buf_addr = stack_top + stack_size;
    auto *buf = (uint8_t *)heap_caps_malloc(page_size, MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA);
    memset(buf, 0, page_size);

    for (uint32_t page_idx = 0; page_idx < ((len / page_size) + ((len % page_size != 0) ? 1 : 0)); page_idx += 1) {
        uint32_t write_size = std::min(page_size, remain_len);
        size_t read_len = fread(buf, 1, write_size, file);
        ESP_LOGD(TAG, "program_file: write size: %lu", write_size);
        if (read_len != write_size) {
            ESP_LOGW(TAG, "Trying to read %lu bytes but got only %u bytes", write_size, read_len);
            write_size = read_len;
        }

        curr_buf_addr = stack_top + stack_size;
        swd_ret = swd_write_memory(curr_buf_addr, (uint8_t *)buf, write_size);
        if (swd_ret < 1) {
            ESP_LOGE(TAG, "Failed when writing RAM cache");
            free(buf);
            state = swd_def::UNKNOWN;
            return ESP_ERR_INVALID_STATE;
        }

        ESP_LOGI(TAG, "Writing page 0x%08lx, size %lu from RAM 0x%08lx", addr_offset + (page_idx * page_size), write_size,
                 curr_buf_addr);
        swd_ret = swd_flash_syscall_exec(&syscall, pc_program_page,
                                         addr_offset + (page_idx * page_size), // r0 = flash base addr
                                         write_size,                           // r1 = length
                                         curr_buf_addr, 0,                     // r2 = buf addr
                                         FLASHALGO_RETURN_BOOL, nullptr);

        if (swd_ret < 1) {
            ESP_LOGE(TAG, "Failed when programming data to target");
            free(buf);
            state = swd_def::UNKNOWN;
            return ESP_ERR_INVALID_STATE;
        }

        if (page_idx % 2 == 0) {
            led.set_color(50, 50, 0);
        } else {
            led.set_color(0, 0, 0);
        }

        remain_len -= write_size;
    }

    free(buf);
    return ESP_OK;
}

void swd_prog::reset_gpio()
{
    swd_off();
}
