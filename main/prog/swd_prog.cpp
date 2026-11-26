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

static constexpr uint32_t DHCSR_ADDR = 0xE000EDF0; // DBG_HCSR is private to swd_host.c

const si::config::flash_algorithm &swd_prog::algo()
{
    return fw_asset_manager::instance()->config().algo;
}

esp_err_t swd_prog::connect_and_halt()
{
    auto ret = swd_init_debug(pdMS_TO_TICKS(1000));
    if (ret == ESP_OK) {
        vTaskDelay(1);
        ret = swd_halt_target();
    }
    if (ret == ESP_OK) {
        ret = swd_wait_until_halted();
    }
    if (ret == ESP_OK) {
        return ESP_OK;
    }

    // Firmware that turns SWD off or sleeps at boot wins that race every time.
    ESP_LOGW(TAG, "connect: failed with 0x%x, retrying under reset", ret);
    return swd_connect_under_reset(pdMS_TO_TICKS(1000));
}

esp_err_t swd_prog::load_flash_algorithm()
{
    algo_resident = false;
    swd_off();
    vTaskDelay(1);
    auto init_err = swd_init(pdMS_TO_TICKS(1000));
    if (init_err != ESP_OK) {
        return init_err;
    }
    vTaskDelay(1);
    swd_trigger_nrst();
    vTaskDelay(1);

    auto ret = connect_and_halt();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "load_algo: Cannot connect to and halt the target");
        state = swd_def::UNKNOWN;
        return ret;
    }

    const si::config::flash_algorithm &fa = algo();
    algo_bin_len = fa.algo_bin_len;
    code_start = fa.load_address;

    // The decoded algorithm blob is already arena-backed (PSRAM); no copy.
    ESP_LOGI(TAG, "load_algo: algo '%s', %zu bytes @ 0x%08lx", fa.name, algo_bin_len, code_start);

    code_end = next_multiple_of((code_start + algo_bin_len),
                                8); // Force align to 8, for 32-bit ARM Cortex-M. I don't know why but probe-rs did this.

    ret = swd_write_word(code_start - sizeof(uint32_t), halt_header);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "load_algo: Failed when writing flash algorithm header");
        state = swd_def::UNKNOWN;
        return ESP_FAIL;
    }

    ret = swd_write_memory(code_start, const_cast<uint8_t *>(fa.algo_bin), algo_bin_len);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "load_algo: Failed when writing main flash algorithm");
        state = swd_def::UNKNOWN;
        return ESP_FAIL;
    }

    // Write stack canary here so it survives target resets between algorithm loads
    ret = swd_write_word(stack_bottom, stack_canary);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "load_algo: Failed when writing stack canary");
        state = swd_def::UNKNOWN;
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "load_algo: OK");
    state = swd_def::FLASH_ALG_LOADED;
    algo_resident = true;
    resident_generation = fw_asset_manager::instance()->config().generation;
    return ESP_OK;
}

esp_err_t swd_prog::reuse_flash_algorithm()
{
    const si::config::flash_algorithm &fa = algo();
    if (!algo_resident || state == swd_def::UNKNOWN || resident_generation != fw_asset_manager::instance()->config().generation ||
        fa.load_address != code_start || fa.algo_bin_len != algo_bin_len) {
        return ESP_ERR_INVALID_STATE;
    }
    algo_resident = false; // until every step below passes

    // Halted, and not reset since the last DHCSR read (S_RESET_ST is sticky
    // until read; the algorithm calls read DHCSR after every call).
    uint32_t dhcsr = 0;
    auto ret = swd_read_word(DHCSR_ADDR, &dhcsr);
    if (ret != ESP_OK || (dhcsr & S_HALT) == 0 || (dhcsr & (S_RESET_ST | S_LOCKUP)) != 0) {
        ESP_LOGW(TAG, "reuse_algo: target not halted as left (DHCSR 0x%08lx, read 0x%x); loading again", (unsigned long)dhcsr, ret);
        return ESP_ERR_INVALID_STATE;
    }

    // The halt header and the stack canary lie outside the image, where only
    // a write from someone else changes them.
    uint32_t header = 0, canary = 0;
    ret = swd_read_word(code_start - sizeof(uint32_t), &header);
    ret = ret ?: swd_read_word(stack_bottom, &canary);
    if (ret != ESP_OK || header != halt_header || canary != stack_canary) {
        ESP_LOGW(TAG, "reuse_algo: halt header 0x%08lx, canary 0x%08lx changed since the algorithm was loaded (read 0x%x); loading again",
                 (unsigned long)header, (unsigned long)canary, ret);
        return ESP_ERR_INVALID_CRC;
    }

    // The image is written again whole: it may hold the algorithm's own
    // variables (this STM32WL one keeps one in the padding after its code,
    // below the yaml's data section), so reading it back can't tell a changed
    // variable from damaged code. Writing it costs what reading it would, and
    // leaves exactly what a reload does, minus the SWD restart and target reset.
    ret = swd_write_memory(code_start, const_cast<uint8_t *>(fa.algo_bin), algo_bin_len);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "reuse_algo: rewriting the algorithm failed (0x%x); loading again", ret);
        return ret;
    }

    state = swd_def::FLASH_ALG_LOADED;
    algo_resident = true;
    return ESP_OK;
}

bool swd_prog::overlaps_algorithm(uint32_t addr, size_t len) const
{
    if (!algo_resident || len == 0) {
        return false;
    }
    const uint64_t start = code_start - sizeof(uint32_t), end = uint64_t(stack_bottom) + sizeof(uint32_t); // header .. canary
    return addr < end && uint64_t(addr) + len > start;
}

esp_err_t swd_prog::run_algo_init(swd_def::init_mode mode)
{
    const si::config::flash_algorithm &fa = algo();

    ESP_LOGI(
        TAG, "algo_init: Running init, load_addr: 0x%lx, stack_ptr: 0x%lx, static_base: 0x%lx", syscall.breakpoint, syscall.stack_pointer,
        syscall.static_base
    );
    uint32_t retry_cnt = 3;
    while (retry_cnt > 0) {
        if (reuse_flash_algorithm() == ESP_OK) {
            ESP_LOGI(TAG, "algo_init: algorithm still loaded, image rewritten");
        } else if (load_flash_algorithm() != ESP_OK) {
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
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "algo_init: Timeout when halting");
            state = swd_def::UNKNOWN;
            return ESP_ERR_INVALID_STATE;
        }

        ESP_LOGD(TAG, "algo_init: Flash start addr = 0x%lx, pc_init = 0x%lx", fa.flash_start.value(), fa.pc_init.value());

        ret = swd_flash_syscall_exec(
            &syscall,
            fa.pc_init.value(),     // Init PC (usually) = 1, +0x20 for header (but somehow actually 0?)
            fa.flash_start.value(), // r0 = flash base addr
            0,                      // r1 = ignored
            mode, 0,                // r2 = mode, r3 ignored
            FLASHALGO_RETURN_BOOL, nullptr
        );

        if (ret != ESP_OK) {
            ESP_LOGW(TAG, "algo_init: Failed when init algorithm, returned %d, retrying...", ret);
            forget_algorithm();
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
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed when halting");
        state = swd_def::UNKNOWN;
        return ESP_ERR_INVALID_STATE;
    }

    if (!fa.pc_uninit.has_value()) {
        ESP_LOGE(TAG, "UnInit function (pc_uninit) not configured");
        return ESP_ERR_NOT_FOUND;
    }

    ret = swd_wait_until_halted();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Timeout when halting");
        state = swd_def::UNKNOWN;
        return ESP_ERR_INVALID_STATE;
    }

    ESP_LOGI(TAG, "Calling uninit, pc=0x%08lx", fa.pc_uninit.value());
    ret = swd_flash_syscall_exec(
        &syscall, fa.pc_uninit.value(), // UnInit PC = 61
        mode, 0, 0, 0,                  // r2, r3 = ignored
        FLASHALGO_RETURN_BOOL, nullptr
    );

    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed when uninit algorithm");
        state = swd_def::UNKNOWN;
        return ESP_FAIL;
    }

    // Check stack canary
    uint32_t curr_stack_canary = 0;
    ret = swd_read_word(stack_bottom, &curr_stack_canary);
    if (ret != ESP_OK) {
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
    const si::config::flash_algorithm &fa_cfg = algo();

    /*
     * Select the RAM region that actually CONTAINS the flash algorithm
     * (load_address .. +algo_bin_len, and the static base when present),
     * not blindly the largest bank: on multi-bank parts the algorithm is
     * linked to one specific bank, and stack/page buffers must live in the
     * same bank above it.
     */
    const si::config::ram_region *ram = cfg.algo_ram_region();
    if (ram == nullptr) {
        ESP_LOGE(
            TAG, "No RAM region in target.yaml contains the flash algorithm (load 0x%08lx, %zu bytes)", fa_cfg.load_address, fa_cfg.algo_bin_len
        );
        return ESP_ERR_NOT_FOUND;
    }

    if (fa_cfg.data_section_offset.has_value() && !ram->contains(fa_cfg.data_section_offset.value())) {
        ESP_LOGW(
            TAG, "data_section (static base) 0x%08lx lies outside the algorithm RAM region 0x%08lx-0x%08lx", fa_cfg.data_section_offset.value(),
            ram->start, ram->end
        );
    }

    /*
     * Capacity check: everything swd_prog places in this region must fit -
     * the algorithm blob, the stack, and the RAM page buffer(s) above the
     * stack (double-buffered programming uses up to two pages). With a
     * 4-byte header word below code_start, aligned up to 8.
     */
    if (!fa_cfg.data_section_offset.has_value()) {
        ESP_LOGE(TAG, "data_section_offset not configured");
        return ESP_ERR_NOT_FOUND;
    }

    if (!fa_cfg.page_size.has_value()) {
        ESP_LOGE(TAG, "page_size not configured");
        return ESP_ERR_NOT_FOUND;
    }

    /*
     * Target RAM layout from load_address:
     *   [code_start - 4 .. code_start)         : 4-byte halt header
     *   [code_start     .. code_end)           : algorithm binary (8-byte aligned)
     *   [code_end       .. stack_bottom)       : 2x page_size slack for .data/.bss
     *   [stack_bottom   .. stack_top)          : algorithm stack (grows downward)
     *   [stack_top      .. stack_top + N*page) : 1 page (simple) or 2 pages (double-buffered)
     */
    stack_size = _stack_size;
    const uint32_t page = fa_cfg.page_size.value();
    const uint32_t code_start_in_region = fa_cfg.load_address;
    const uint32_t planned_code_end = next_multiple_of(code_start_in_region + fa_cfg.algo_bin_len, 8);
    // TODO: stack_bottom still may not be correct, it probably should consider the SelfTestInfo section, not just simply add two flash pages.
    const uint32_t planned_stack_bottom = next_multiple_of(planned_code_end + page * 2, 8);
    const uint32_t planned_stack_top = planned_stack_bottom + stack_size;
    const uint32_t min_end = planned_stack_top + page;
    if (min_end < code_start_in_region || !ram->contains_range(code_start_in_region, min_end - code_start_in_region)) {
        ESP_LOGE(
            TAG, "Algorithm RAM region 0x%08lx-0x%08lx too small: need 0x%08lx-0x%08lx (algo %zu + stack %lu + pages %lu)", ram->start, ram->end,
            code_start_in_region, min_end, fa_cfg.algo_bin_len, stack_size, page
        );
        return ESP_ERR_NO_MEM;
    }

    ram_start_addr = ram->start;
    ram_size = ram->size();
    stack_bottom = planned_stack_bottom;
    stack_top = planned_stack_top;
    stack_canary = esp_random();

    ESP_LOGI(TAG, "Algorithm RAM region 0x%08lx-0x%08lx (selected by containment, %zu regions in map)", ram->start, ram->end, cfg.ram_region_count);
    ESP_LOGI(TAG, "Init target");
    auto ret = swd_init_debug(pdMS_TO_TICKS(1000));
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed when init");
        state = swd_def::UNKNOWN;
        return ret;
    }

    ESP_LOGI(TAG, "Halt target");
    ret = swd_halt_target();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed when halting");
        state = swd_def::UNKNOWN;
        return ESP_ERR_INVALID_STATE;
    }

    ret = swd_wait_until_halted();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Timeout when halting");
        state = swd_def::UNKNOWN;
        return ESP_ERR_INVALID_STATE;
    }

    if (load_flash_algorithm() != ESP_OK) {
        ESP_LOGE(TAG, "Failed when loading flash algorithm");
        return ESP_FAIL;
    }

    const si::config::flash_algorithm &fa = algo();

    ESP_LOGI(TAG, "Stack: size=%lu top=0x%08lx, bottom=0x%08lx, canary=0x%08lx", stack_size, stack_top, stack_bottom, stack_canary);

    syscall.breakpoint =
        code_start - sizeof(uint32_t) + 1;                // Breakpoint is where the halt word (2x of breakpoint instructions) + 1 byte for thumb mode
    syscall.static_base = fa.data_section_offset.value(); // BSS, also don't forget the header = 32 bytes
    syscall.stack_pointer = stack_top;

    ESP_LOGI(TAG, "Addr: ram_start_addr: 0x%08lx; data_section: 0x%08lx", ram_start_addr, syscall.static_base);
    ESP_LOGI(
        TAG, "Addr: stack top: 0x%08lx, bottom: 0x%08lx breakpoint: 0x%08lx, static_base: 0x%08lx", stack_top, stack_bottom, syscall.breakpoint,
        syscall.static_base
    );

    state = swd_def::INITIALISED;
    have_layout = true;
    layout_generation = cfg.generation;
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
    if (swd_ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed when init");
        state = swd_def::UNKNOWN;
        return ESP_ERR_INVALID_STATE;
    }

    swd_ret = swd_wait_until_halted();
    if (swd_ret != ESP_OK) {
        ESP_LOGE(TAG, "Timeout when halting");
        state = swd_def::UNKNOWN;
        return ESP_ERR_INVALID_STATE;
    }

    led.set_color(0, 0, 60);

    ESP_LOGI(TAG, "Calling chip erase...");

    swd_ret = swd_flash_syscall_exec(
        &syscall, fa.pc_erase_all.value(), 0, // No arguments
        0, 0, 0,                              // r1, r2 = ignored
        FLASHALGO_RETURN_BOOL, nullptr
    );

    if (swd_ret != ESP_OK) {
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
    if (swd_ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed when halting");
        state = swd_def::UNKNOWN;
        return ESP_ERR_INVALID_STATE;
    }

    swd_ret = swd_wait_until_halted();
    if (swd_ret != ESP_OK) {
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
        FLASHALGO_RETURN_VALUE, func_return_val
    );

    if (swd_ret != ESP_OK) {
        ESP_LOGE(TAG, "Self-test function returned an unknown error");
        state = swd_def::UNKNOWN;
        return ESP_FAIL;
    }

    return ESP_OK;
}

esp_err_t swd_prog::erase_sector(uint32_t start_addr, uint32_t end_addr)
{
    const si::config::flash_algorithm &fa = algo();

    // Every sector of the job's sector map (page_size sectors without one)
    // that holds a byte of [start_addr, end_addr).
    uint32_t first = 0, first_size = 0;
    if (!fa.pc_erase_sector.has_value() || end_addr <= start_addr || !fa.sector_at(start_addr, &first, &first_size)) {
        ESP_LOGE(TAG, "erase_sector: no EraseSector, or 0x%08lx-0x%08lx isn't in the flash", start_addr, end_addr);
        return ESP_ERR_INVALID_STATE;
    }
    ESP_LOGI(TAG, "erase_sector: 0x%08lx-0x%08lx, first sector 0x%08lx (%lu bytes)", start_addr, end_addr, first, first_size);

    if (state != swd_def::FLASH_ALG_INITED) {
        ESP_LOGW(TAG, "Flash alg not initialised, doing now");
        auto ret = run_algo_init(swd_def::ERASE);
        if (ret != ESP_OK)
            return ret;
    }

    auto swd_ret = swd_halt_target();
    if (swd_ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed when init");
        state = swd_def::UNKNOWN;
        return ESP_ERR_INVALID_STATE;
    }

    swd_ret = swd_wait_until_halted();
    if (swd_ret != ESP_OK) {
        ESP_LOGE(TAG, "Timeout when halting");
        state = swd_def::UNKNOWN;
        return ESP_ERR_INVALID_STATE;
    }

    uint32_t idx = 0, sector = 0, size = 0;
    for (uint32_t a = start_addr; a < end_addr && fa.sector_at(a, &sector, &size); a = sector + size, idx++) {
        swd_ret = swd_flash_syscall_exec(&syscall, fa.pc_erase_sector.value(), sector, 0, 0, 0, FLASHALGO_RETURN_BOOL, nullptr);
        if (swd_ret != ESP_OK) {
            ESP_LOGE(TAG, "erase_sector: sector 0x%08lx failed: 0x%x", sector, swd_ret);
            state = swd_def::UNKNOWN;
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
    if (swd_ret != ESP_OK) {
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
    if (swd_ret != ESP_OK) {
        ESP_LOGE(TAG, "Timeout when halting");
        state = swd_def::UNKNOWN;
        return ESP_ERR_INVALID_STATE;
    }

    ESP_LOGI(
        TAG, "program_page: page_size: %lu, pc_prg_page=0x%lx, flash_start_addr=0x%lx", fa.page_size.value(), fa.pc_program_page.value(),
        fa.flash_start.value()
    );

    uint32_t addr_offset = fa.flash_start.value() + (start_addr == UINT32_MAX ? 0 : start_addr);
    uint32_t remain_len = len;
    for (uint32_t page_idx = 0; page_idx < (len / fa.page_size.value()); page_idx += 1) {
        uint32_t write_size = std::min(fa.page_size.value(), remain_len);
        ESP_LOGD(TAG, "program_page: write size: %lu", write_size);
        swd_ret = swd_write_memory(stack_top, (uint8_t *)(buf + (page_idx * fa.page_size.value())), write_size);
        if (swd_ret != ESP_OK) {
            ESP_LOGE(TAG, "Failed when writing RAM cache");
            state = swd_def::UNKNOWN;
            return ESP_ERR_INVALID_STATE;
        }

        ESP_LOGD(TAG, "Writing page 0x%lx, size %lu", addr_offset + (page_idx * fa.page_size.value()), write_size);
        swd_ret = swd_flash_syscall_exec(
            &syscall, fa.pc_program_page.value(),            // ErasePage PC = 305
            addr_offset + (page_idx * fa.page_size.value()), // r0 = flash base addr
            write_size, stack_top, 0,                        // r1 = len, r2 = buf addr
            FLASHALGO_RETURN_BOOL, nullptr
        );

        if (page_idx % 2 == 0) {
            led.set_color(50, 50, 0);
        } else {
            led.set_color(0, 0, 0);
        }

        remain_len -= write_size;
    }

    if (swd_ret != ESP_OK) {
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
    ESP_LOGD(
        TAG, "program_file: page_size: %lu, pc_prg_page=0x%lx, flash_start_addr=0x%lx", fa.page_size.value(), fa.pc_program_page.value(),
        fa.flash_start.value()
    );

    esp_err_t ret = ESP_OK;
    uint32_t max_possible_buffer_addr = stack_top + fa.page_size.value() * 2;
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
    if (swd_ret != ESP_OK) {
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
        if (swd_ret != ESP_OK) {
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

esp_err_t swd_prog::perform_double_buffered_program(FILE *file, uint32_t len, uint32_t page_size, uint32_t pc_program_page, uint32_t addr_offset)
{
    uint32_t remain_len = len;
    esp_err_t swd_ret = ESP_OK;
    uint8_t curr_buf = 0;
    uint32_t curr_buf_addr = stack_top;
    auto *buf = (uint8_t *)heap_caps_malloc(page_size, MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA);
    if (buf == nullptr) {
        ESP_LOGE(TAG, "perform_double_buffered_program: out of memory for %lu-byte page buffer", page_size);
        return ESP_ERR_NO_MEM;
    }
    memset(buf, 0, page_size);

    for (uint32_t page_idx = 0; page_idx < ((len / page_size) + ((len % page_size != 0) ? 1 : 0)); page_idx += 1) {
        uint32_t write_size = std::min(page_size, remain_len);
        size_t read_len = fread(buf, 1, write_size, file);
        ESP_LOGD(TAG, "program_file: write size: %lu", write_size);
        if (read_len != write_size) {
            ESP_LOGW(TAG, "Trying to read %lu bytes but got only %u bytes", write_size, read_len);
            write_size = read_len;
        }

        curr_buf_addr = curr_buf == 0 ? stack_top : (stack_top + page_size);
        swd_ret = swd_write_memory(curr_buf_addr, (uint8_t *)buf, write_size);
        if (swd_ret != ESP_OK) {
            ESP_LOGE(TAG, "Failed when writing RAM cache");
            free(buf);
            state = swd_def::UNKNOWN;
            return ESP_ERR_INVALID_STATE;
        }

        swd_ret = swd_flash_syscall_wait_result(FLASHALGO_RETURN_BOOL, nullptr);
        if (swd_ret != ESP_OK) {
            ESP_LOGE(TAG, "Failed when checking programming state");
            free(buf);
            state = swd_def::UNKNOWN;
            return ESP_ERR_INVALID_STATE;
        }

        ESP_LOGD(TAG, "Writing page 0x%08lx, size %lu from RAM 0x%08lx", addr_offset + (page_idx * page_size), write_size, curr_buf_addr);
        swd_ret = swd_flash_syscall_exec_async(
            &syscall, pc_program_page,
            addr_offset + (page_idx * page_size), // r0 = flash base addr
            write_size,                           // r1 = length
            curr_buf_addr, 0                      // r2 = buf addr
        );

        if (swd_ret != ESP_OK) {
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
        if (on_progress != nullptr) {
            on_progress(len - remain_len, len);
        }
    }

    swd_ret = swd_flash_syscall_wait_result(FLASHALGO_RETURN_BOOL, nullptr);
    if (swd_ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed when checking programming state after finish");
        free(buf);
        state = swd_def::UNKNOWN;
        return ESP_ERR_INVALID_STATE;
    }

    free(buf);
    return ESP_OK;
}

esp_err_t swd_prog::perform_simple_program(FILE *file, uint32_t len, uint32_t page_size, uint32_t pc_program_page, uint32_t addr_offset)
{
    uint32_t remain_len = len;
    esp_err_t swd_ret = ESP_OK;
    uint32_t curr_buf_addr = stack_top;
    auto *buf = (uint8_t *)heap_caps_malloc(page_size, MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA);
    if (buf == nullptr) {
        ESP_LOGE(TAG, "perform_simple_program: out of memory for %lu-byte page buffer", page_size);
        return ESP_ERR_NO_MEM;
    }
    memset(buf, 0, page_size);

    for (uint32_t page_idx = 0; page_idx < ((len / page_size) + ((len % page_size != 0) ? 1 : 0)); page_idx += 1) {
        uint32_t write_size = std::min(page_size, remain_len);
        size_t read_len = fread(buf, 1, write_size, file);
        ESP_LOGD(TAG, "program_file: write size: %lu", write_size);
        if (read_len != write_size) {
            ESP_LOGW(TAG, "Trying to read %lu bytes but got only %u bytes", write_size, read_len);
            write_size = read_len;
        }

        curr_buf_addr = stack_top;
        swd_ret = swd_write_memory(curr_buf_addr, (uint8_t *)buf, write_size);
        if (swd_ret != ESP_OK) {
            ESP_LOGE(TAG, "Failed when writing RAM cache");
            free(buf);
            state = swd_def::UNKNOWN;
            return ESP_ERR_INVALID_STATE;
        }

        ESP_LOGI(TAG, "Writing page 0x%08lx, size %lu from RAM 0x%08lx", addr_offset + (page_idx * page_size), write_size, curr_buf_addr);
        swd_ret = swd_flash_syscall_exec(
            &syscall, pc_program_page,
            addr_offset + (page_idx * page_size), // r0 = flash base addr
            write_size,                           // r1 = length
            curr_buf_addr, 0,                     // r2 = buf addr
            FLASHALGO_RETURN_BOOL, nullptr
        );

        if (swd_ret != ESP_OK) {
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
        if (on_progress != nullptr) {
            on_progress(len - remain_len, len);
        }
    }

    free(buf);
    return ESP_OK;
}

void swd_prog::reset_gpio()
{
    swd_off();
}


esp_err_t swd_prog::ensure_layout()
{
    if (have_layout && layout_generation == fw_asset_manager::instance()->config().generation) {
        return ESP_OK;
    }
    return init();
}

bool swd_prog::read_has_fpu()
{
    static constexpr uint32_t MVFR0_ADDR = 0xE000EF40;
    uint32_t mvfr0 = 0;
    if (swd_read_word(MVFR0_ADDR, &mvfr0) != ESP_OK) {
        (void)swd_clear_errors(); // ARMv6-M has no MVFR0
        return false;
    }
    return (mvfr0 & 0xF0) != 0; // single precision support
}

esp_err_t swd_prog::check_flash_args(flash_access op, uint32_t addr, uint32_t len, char *why, size_t why_len)
{
    const si::config::flash_algorithm &fa = algo();
    if (!fa.flash_start.has_value() || !fa.flash_end.has_value() || !fa.page_size.has_value() || fa.page_size.value() == 0) {
        ESP_LOGE(TAG, "flash: the algorithm has no flash range or page size");
        return ESP_ERR_INVALID_STATE;
    }
    if ((op == flash_access::erase && !fa.pc_erase_sector.has_value()) ||
        (op == flash_access::write && !fa.pc_program_page.has_value())) {
        ESP_LOGE(TAG, "flash: the algorithm has no %s function", op == flash_access::erase ? "EraseSector" : "ProgramPage");
        return ESP_ERR_INVALID_STATE;
    }
    const uint32_t start = fa.flash_start.value(), end = fa.flash_end.value();
    if (op == flash_access::erase) { // len counts sectors
        uint32_t s = 0, size = 0;
        if (len == 0) {
            snprintf(why, why_len, "0 sectors");
        } else if (!fa.sector_at(addr, &s, &size)) {
            snprintf(why, why_len, "0x%08lx is outside the flash, 0x%08lx-0x%08lx", (unsigned long)addr, (unsigned long)start,
                     (unsigned long)end);
        } else if (s != addr) {
            snprintf(why, why_len, "0x%08lx isn't the start of a sector (that one is 0x%08lx, %lu bytes)", (unsigned long)addr,
                     (unsigned long)s, (unsigned long)size);
        } else {
            uint32_t a = addr, n = 0;
            while (n < len && fa.sector_at(a, &s, &size)) {
                a = s + size;
                n++;
            }
            if (n == len) {
                return ESP_OK;
            }
            snprintf(why, why_len, "%lu sectors from 0x%08lx run past the end of the flash (%lu fit)", (unsigned long)len,
                     (unsigned long)addr, (unsigned long)n);
        }
        return ESP_ERR_INVALID_ARG;
    }
    if (len == 0) {
        snprintf(why, why_len, "length 0");
    } else if (addr < start || addr >= end || len > end - addr) {
        snprintf(why, why_len, "0x%08lx+%lu is outside the flash, 0x%08lx-0x%08lx", (unsigned long)addr, (unsigned long)len,
                 (unsigned long)start, (unsigned long)end);
    } else if (op == flash_access::write && len % 4 != 0) {
        snprintf(why, why_len, "length %lu isn't a multiple of 4", (unsigned long)len);
    } else {
        return ESP_OK;
    }
    return ESP_ERR_INVALID_ARG;
}

esp_err_t swd_prog::erase_sectors(uint32_t addr, uint32_t count)
{
    char why[96] = "";
    auto ret = ensure_layout();
    ret = ret ?: check_flash_args(flash_access::erase, addr, count, why, sizeof(why));
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "erase_sectors: %s", why[0] ? why : esp_err_to_name(ret));
        return ret;
    }
    const si::config::flash_algorithm &fa = algo();

    ret = run_algo_init(swd_def::ERASE);
    if (ret != ESP_OK) {
        return ret;
    }
    uint32_t a = addr, s = 0, size = 0;
    for (uint32_t n = 0; n < count && fa.sector_at(a, &s, &size); n++, a = s + size) {
        ret = swd_flash_syscall_exec(&syscall, fa.pc_erase_sector.value(), s, 0, 0, 0, FLASHALGO_RETURN_BOOL, nullptr);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "erase_sectors: sector 0x%08lx failed: 0x%x", s, ret);
            state = swd_def::UNKNOWN;
            return ret;
        }
    }
    return run_algo_uninit(swd_def::ERASE);
}

esp_err_t swd_prog::write_range(uint32_t addr, const uint8_t *buf, uint32_t len)
{
    char why[80] = "";
    auto ret = ensure_layout();
    ret = ret ?: check_flash_args(flash_access::write, addr, len, why, sizeof(why));
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "write_range: %s", why[0] ? why : esp_err_to_name(ret));
        return ret;
    }
    const si::config::flash_algorithm &fa = algo();

    ret = run_algo_init(swd_def::PROGRAM);
    if (ret != ESP_OK) {
        return ret;
    }
    const uint32_t page = fa.page_size.value(), base = fa.flash_start.value();
    for (uint32_t done = 0; done < len;) {
        uint32_t a = addr + done;
        uint32_t n = std::min(page - (a - base) % page, len - done);
        ret = swd_write_memory(stack_top, const_cast<uint8_t *>(buf + done), n);
        ret = ret ?: swd_flash_syscall_exec(&syscall, fa.pc_program_page.value(), a, n, stack_top, 0, FLASHALGO_RETURN_BOOL, nullptr);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "write_range: %lu bytes at 0x%08lx failed: 0x%x", n, a, ret);
            state = swd_def::UNKNOWN;
            return ret;
        }
        done += n;
    }
    return run_algo_uninit(swd_def::PROGRAM);
}

esp_err_t swd_prog::call(uint32_t addr, const uint32_t args[4], uint32_t stack_top_in, uint32_t timeout_ms, bool (*cancelled)(), uint32_t *r0)
{
    const uint32_t bkpt = stack_top_in - 8; // returns here, stack below
    const si::config::target_config &cfg = fw_asset_manager::instance()->config();
    bool in_ram = false;
    for (size_t i = 0; i < cfg.ram_region_count && !in_ram; i++) {
        in_ram = cfg.ram_regions[i].contains_range(bkpt, 8);
    }
    if ((stack_top_in & 7) != 0 || !in_ram) {
        ESP_LOGE(TAG, "call: stack top 0x%08lx isn't 8-aligned inside a RAM region of the job", stack_top_in);
        return ESP_ERR_INVALID_ARG;
    }
    forget_algorithm(); // the target's own code runs, and may use any RAM

    auto ret = swd_halt_target();
    ret = ret ?: swd_wait_until_halted();
    ret = ret ?: swd_write_word(bkpt, halt_header);
    if (ret != ESP_OK) {
        return ret;
    }
    const program_syscall_t sc = {.breakpoint = bkpt | 1, .static_base = 0, .stack_pointer = bkpt};
    ret = swd_flash_syscall_exec_async(&sc, addr | 1, args[0], args[1], args[2], args[3]); // interrupts masked
    if (ret != ESP_OK) {
        return ret;
    }

    const int64_t until = esp_timer_get_time() + timeout_ms * 1000LL;
    for (;;) {
        uint32_t dhcsr = 0;
        ret = swd_read_word(DHCSR_ADDR, &dhcsr);
        if (ret != ESP_OK) {
            return ret;
        }
        if (dhcsr & S_HALT) {
            break;
        }
        bool late = esp_timer_get_time() >= until;
        if (late || (cancelled != nullptr && cancelled())) {
            swd_halt_target(); // leave it halted
            ESP_LOGW(TAG, "call: 0x%08lx %s", addr, late ? "timed out" : "cancelled");
            return late ? ESP_ERR_TIMEOUT : ESP_ERR_NOT_FINISHED;
        }
        vTaskDelay(1);
    }

    uint32_t pc = 0;
    ret = swd_read_core_register(15, &pc);
    ret = ret ?: swd_read_core_register(0, r0);
    ret = ret ?: swd_write_word(DHCSR_ADDR, DBGKEY | C_DEBUGEN | C_HALT); // drop C_MASKINTS, stay halted
    if (ret != ESP_OK) {
        return ret;
    }
    if ((pc & ~1u) != bkpt) {
        ESP_LOGE(TAG, "call: halted at 0x%08lx, not at the return breakpoint 0x%08lx (fault or BKPT?)", pc, bkpt);
        return ESP_FAIL;
    }
    return ESP_OK;
}
