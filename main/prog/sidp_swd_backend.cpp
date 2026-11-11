#include "sidp_swd_backend.hpp"

#include <cstring>

#include <driver/gpio.h>
#include <esp_log.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <swd_host.h>

#include "fw_asset_manager.hpp"
#include "job_controller.hpp"
#include "target_lines.hpp"

static const constexpr uint32_t CORE_REG_PC = 15;
static const constexpr uint32_t CORE_REG_SPECIAL = 20; // CONTROL, FAULTMASK, BASEPRI and PRIMASK, one byte each
static const constexpr uint32_t CORE_REG_FPSCR = 33;

static const constexpr size_t REG_ENTRY_SIZE = sizeof(sidp::register_value_t) + sizeof(uint32_t);
static const constexpr size_t REG_SEEN_WORDS = sidp::ARM_REG_FPSCR / 32 + 1;

static const constexpr uint32_t FP_CTRL_ADDR = 0xE0002000;
static const constexpr uint32_t FP_COMP_ADDR = 0xE0002008;
static const constexpr uint32_t CPUID_ADDR = 0xE000ED00;
static const constexpr uint32_t AIRCR_ADDR = 0xE000ED0C;
static const constexpr uint32_t DFSR_ADDR = 0xE000ED30;
static const constexpr uint32_t DHCSR_ADDR = 0xE000EDF0;
static const constexpr uint32_t DEMCR_ADDR = 0xE000EDFC;
static const constexpr uint32_t MVFR0_ADDR = 0xE000EF40;

static const constexpr uint32_t CPUID_IMPLEMENTER_ARM = 0x41;
static const constexpr uint32_t CPUID_PART_M0 = 0xC20;
static const constexpr uint32_t CPUID_PART_M0P = 0xC60;
static const constexpr uint32_t CPUID_PART_M3 = 0xC23;
static const constexpr uint32_t CPUID_PART_M4 = 0xC24;

static const constexpr uint32_t DFSR_BKPT = 1u << 1;
static const constexpr uint32_t DFSR_CLEAR_ALL = 0x1F;

static const constexpr uint32_t FP_CTRL_KEY = 1u << 1;
static const constexpr uint32_t FP_CTRL_ENABLE = 1u << 0;
static const constexpr uint32_t FP_COMP_ENABLE = 1u << 0;
static const constexpr uint32_t FP_COMP_REPLACE_LOWER = 1u << 30;
static const constexpr uint32_t FP_COMP_REPLACE_UPPER = 2u << 30;
static const constexpr uint32_t FP_COMP_ADDR_MASK = 0x1FFFFFFC;
static const constexpr uint32_t FPB_CODE_LIMIT = 0x20000000; // Breakpoints only match below this address

// The session treats only ESP_FAIL as a broken link, so a bad ACK must be one too.
static esp_err_t link_error(esp_err_t ret)
{
    return ret == ESP_ERR_INVALID_RESPONSE ? ESP_FAIL : ret;
}

// A FAULT ACK on a memory access means a bad address, not a broken link.
static esp_err_t memory_error(esp_err_t ret)
{
    if (ret != ESP_FAIL) {
        return link_error(ret);
    }
    return swd_clear_errors() == ESP_OK ? ESP_ERR_INVALID_ARG : ESP_FAIL;
}

static bool is_special_reg(uint16_t id)
{
    return id >= sidp::ARM_REG_PRIMASK && id <= sidp::ARM_REG_CONTROL;
}

static uint32_t special_reg_shift(uint16_t id)
{
    return (id - sidp::ARM_REG_PRIMASK) * 8;
}

// The core register selector matches the SIDP ID for everything except FPSCR.
static uint32_t core_reg_selector(uint16_t id)
{
    return id == sidp::ARM_REG_FPSCR ? CORE_REG_FPSCR : id;
}

static void drive_nrst(bool asserted)
{
#ifdef CONFIG_ESP_SWD_PHY_AXC2T245
    // A high GPIO turns on the reset MOSFET.
    gpio_set_level(static_cast<gpio_num_t>(CONFIG_ESP_SWD_NRST_PIN), asserted ? 1 : 0);
#else
    gpio_set_level(static_cast<gpio_num_t>(CONFIG_ESP_SWD_NRST_PIN), asserted ? 0 : 1);
#endif
}

static uint32_t fpb_comparator(uint32_t address)
{
    const uint32_t replace = (address & 2) != 0 ? FP_COMP_REPLACE_UPPER : FP_COMP_REPLACE_LOWER;
    return replace | (address & FP_COMP_ADDR_MASK) | FP_COMP_ENABLE;
}

// Records id in the seen set. Returns true if it was already there.
static bool mark_seen(uint32_t *seen, uint16_t id)
{
    const uint32_t bit = 1u << (id % 32);
    const bool duplicate = (seen[id / 32] & bit) != 0;
    seen[id / 32] |= bit;
    return duplicate;
}

esp_err_t sidp_swd_backend::load_target()
{
    static_assert(MAX_REGIONS >= 1 + si::config::target_config::MAX_RAM_REGIONS);

    auto *asset = fw_asset_manager::instance();
    if (!asset->has_job() || asset->config().family != si::config::target_family::swd_cortex_m || !asset->config().has_algo) {
        ESP_LOGE(TAG, "load_target: the device needs a Cortex-M job to debug");
        return ESP_ERR_INVALID_STATE;
    }

    const auto &cfg = asset->config();
    if (!cfg.algo.flash_start.has_value() || !cfg.algo.flash_end.has_value()) {
        ESP_LOGE(TAG, "load_target: the job has no flash address range");
        return ESP_ERR_INVALID_STATE;
    }

    const auto flash_flags = static_cast<sidp::memory_flag_t>(sidp::MEM_READ | sidp::MEM_EXECUTE);
    const auto ram_flags = static_cast<sidp::memory_flag_t>(sidp::MEM_READ | sidp::MEM_WRITE | sidp::MEM_EXECUTE | sidp::MEM_CACHEABLE);

    region_count = 0;
    memory_regions[region_count++] = {*cfg.algo.flash_start, *cfg.algo.flash_end - *cfg.algo.flash_start, sidp::MEMORY_FLASH, flash_flags, {0, 0}};
    for (size_t i = 0; i < cfg.ram_region_count; i++) {
        memory_regions[region_count++] = {cfg.ram_regions[i].start, cfg.ram_regions[i].size(), sidp::MEMORY_RAM, ram_flags, {0, 0}};
    }

    memcpy(&target_id, asset->job_sha256(), sizeof(target_id));
    return ESP_OK;
}

esp_err_t sidp_swd_backend::attach(const sidp::attach_params_t &params, sidp::attach_info_t &info)
{
    if (connected) {
        ESP_LOGE(TAG, "attach: already connected");
        return ESP_ERR_INVALID_STATE;
    }

    auto ret = load_target();
    ret = ret ?: claim_target();
    if (ret != ESP_OK) {
        return ret;
    }

    ret = params.connect_under_reset ? connect_under_reset(params.halt_after_attach)
                                     : swd_init_debug(pdMS_TO_TICKS(SWD_LOCK_TIMEOUT_MS));
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "attach: cannot connect: %s", esp_err_to_name(ret));
        swd_off(); // Harmless when swd_host has already dropped the session
        release_target();
        return link_error(ret);
    }

    ret = read_target_info(info);
    if (ret != ESP_OK) {
        swd_off();
        release_target();
        return ret;
    }

    connected = true;
    return ESP_OK;
}

esp_err_t sidp_swd_backend::claim_target()
{
    auto ret = job_controller::instance()->begin_debug();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "claim_target: a programming run is in progress");
        return ret;
    }

    // SWD needs the full CPU speed, the same as a programming run.
    if (pm_lock == nullptr) {
        ret = esp_pm_lock_create(ESP_PM_CPU_FREQ_MAX, 0, "sidp_dbg", &pm_lock);
    }
    ret = ret ?: esp_pm_lock_acquire(pm_lock);
    if (ret != ESP_OK) {
        job_controller::instance()->end_debug();
        return ret;
    }

    target_lines::connect();
    return ESP_OK;
}

void sidp_swd_backend::release_target()
{
    target_lines::isolate();
    esp_pm_lock_release(pm_lock);
    job_controller::instance()->end_debug();
}

esp_err_t sidp_swd_backend::connect_under_reset(bool halt_after)
{
    mask_bits = 0;
    auto ret = swd_connect_under_reset(pdMS_TO_TICKS(SWD_LOCK_TIMEOUT_MS));
    if (ret != ESP_OK || halt_after) {
        return ret;
    }
    return write_control(0);
}

esp_err_t sidp_swd_backend::read_target_info(sidp::attach_info_t &info)
{
    uint32_t cpuid = 0;
    uint32_t dhcsr = 0;
    auto ret = swd_read_word(CPUID_ADDR, &cpuid);
    ret = ret ?: swd_read_word(DHCSR_ADDR, &dhcsr);
    if (ret != ESP_OK) {
        return link_error(ret);
    }

    const uint32_t part = (cpuid >> 4) & 0xFFF;
    if ((cpuid >> 24) != CPUID_IMPLEMENTER_ARM) {
        ESP_LOGE(TAG, "read_target_info: unsupported CPUID 0x%08lx", cpuid);
        return ESP_ERR_NOT_SUPPORTED;
    }

    switch (part) {
    case CPUID_PART_M0:
    case CPUID_PART_M0P:
        profile = sidp::PROFILE_ARMV6M;
        break;
    case CPUID_PART_M3:
        profile = sidp::PROFILE_ARMV7M;
        break;
    case CPUID_PART_M4:
        profile = sidp::PROFILE_ARMV7EM;
        break;
    default:
        ESP_LOGE(TAG, "read_target_info: unsupported CPUID 0x%08lx", cpuid);
        return ESP_ERR_NOT_SUPPORTED;
    }

    // Enable halting debug without resuming a target that is already halted.
    mask_bits = dhcsr & C_MASKINTS;
    ret = swd_write_word(DHCSR_ADDR, DBGKEY | C_DEBUGEN | mask_bits | ((dhcsr & S_HALT) != 0 ? C_HALT : 0));
    if (ret == ESP_OK && (dhcsr & S_HALT) == 0) {
        // Old stop causes must not be mistaken for the next stop.
        ret = swd_write_word(DFSR_ADDR, DFSR_CLEAR_ALL);
    }
    if (ret != ESP_OK) {
        return link_error(ret);
    }

    ret = setup_comparators();
    if (ret != ESP_OK) {
        return link_error(ret);
    }

    uint32_t capabilities = sidp::CAP_STOP_SNAPSHOT | sidp::CAP_SINGLE_STEP | sidp::CAP_RESET_HALT |
                            sidp::CAP_RESET_RUN | sidp::CAP_RESET_SYSTEM | sidp::CAP_RESET_NRST;
    if (comparator_count != 0) {
        capabilities |= sidp::CAP_HARDWARE_BP;
    }

    // Only a Cortex-M4 can have one. A failed probe counts as no FPU.
    uint32_t mvfr0 = 0;
    has_fpu = profile == sidp::PROFILE_ARMV7EM && swd_read_word(MVFR0_ADDR, &mvfr0) == ESP_OK && (mvfr0 & 0xF0) != 0;
    if (has_fpu) {
        capabilities |= sidp::CAP_FPU;
    }

    info.architecture = sidp::ARCH_ARM_M;
    info.profile = profile;
    info.address_width = 32;
    info.capabilities = static_cast<sidp::capability_t>(capabilities);
    info.target_id = target_id;
    info.cpu_id = cpuid;
    info.hardware_breakpoints = comparator_count;
    info.memory_regions = {reinterpret_cast<const uint8_t *>(memory_regions), region_count * sizeof(sidp::memory_region_t)};
    return ESP_OK;
}

esp_err_t sidp_swd_backend::detach(sidp::detach_action_t action)
{
    if (!connected) {
        return ESP_OK;
    }

    auto ret = disable_comparators();
    ret = ret ?: set_reset_catch(false);
    if (ret == ESP_OK && action == sidp::DETACH_RESUME) {
        // The interrupt mask may only change while halted. Clearing C_DEBUGEN then resumes.
        mask_bits = 0;
        ret = write_control(C_HALT);
        if (ret == ESP_OK) {
            // Firmware may turn SWD off as soon as it runs, before this write is acknowledged.
            (void)swd_write_word(DHCSR_ADDR, DBGKEY);
        }
    }
    if (ret != ESP_OK) {
        return link_error(ret);
    }

    connected = false;
    ret = swd_off();
    release_target();
    return ret;
}

esp_err_t sidp_swd_backend::set_reset_catch(bool enable)
{
    // Only touch the catch bit: firmware may be using the trace enable in this register.
    uint32_t demcr = 0;
    auto ret = swd_read_word(DEMCR_ADDR, &demcr);
    demcr = enable ? (demcr | VC_CORERESET) : (demcr & ~VC_CORERESET);
    return ret ?: swd_write_word(DEMCR_ADDR, demcr);
}

esp_err_t sidp_swd_backend::reset(sidp::reset_kind_t kind, bool halt_after, sidp::stop_detect_t &stop)
{
    // The session halts the target first, so the interrupt mask can be cleared here.
    mask_bits = 0;
    auto ret = write_control(C_HALT);
    ret = ret ?: swd_write_word(DFSR_ADDR, DFSR_CLEAR_ALL);
    ret = ret ?: set_reset_catch(halt_after);
    if (ret != ESP_OK) {
        return link_error(ret);
    }

    if (kind == sidp::RESET_NRST) {
        pulse_nrst();
    } else {
        // The core may reset before acknowledging this write. finish_reset() confirms it.
        (void)swd_write_word(AIRCR_ADDR, VECTKEY | SYSRESETREQ);
    }

    vTaskDelay(pdMS_TO_TICKS(RESET_SETTLE_MS));
    return finish_reset(halt_after, stop);
}

void sidp_swd_backend::pulse_nrst()
{
    drive_nrst(true);
    vTaskDelay(pdMS_TO_TICKS(RESET_PULSE_MS));
    drive_nrst(false);
}

esp_err_t sidp_swd_backend::finish_reset(bool halt_after, sidp::stop_detect_t &stop)
{
    const int64_t deadline_us = esp_timer_get_time() + RESET_TIMEOUT_MS * 1000LL;
    while (esp_timer_get_time() < deadline_us) {
        auto ret = poll_halted(stop);
        if (ret == ESP_OK && (stop.halted || !halt_after)) {
            ret = set_reset_catch(false);
            if (!halt_after) {
                ret = ret ?: write_control(0);
            }
            return link_error(ret);
        }

        if (ret != ESP_OK) {
            swd_clear_errors(); // The debug port can report errors during the reset
        }
        vTaskDelay(1);
    }

    return ESP_ERR_TIMEOUT;
}

esp_err_t sidp_swd_backend::write_control(uint32_t bits)
{
    return swd_write_word(DHCSR_ADDR, DBGKEY | C_DEBUGEN | mask_bits | bits);
}

esp_err_t sidp_swd_backend::poll_halted(sidp::stop_detect_t &stop)
{
    stop = sidp::stop_detect_t{};

    uint32_t dhcsr = 0;
    auto ret = swd_read_word(DHCSR_ADDR, &dhcsr);
    if (ret == ESP_OK && (dhcsr & (S_HALT | S_LOCKUP)) == S_LOCKUP) {
        // A locked-up core does not halt by itself, so halt it to report the stop.
        ret = write_control(C_HALT);
        ret = ret ?: swd_read_word(DHCSR_ADDR, &dhcsr);
    }
    if (ret != ESP_OK) {
        return link_error(ret);
    }
    if ((dhcsr & S_HALT) == 0) {
        return ESP_OK;
    }

    stop.halted = true;
    stop.dhcsr = dhcsr;
    stop.lockup = (dhcsr & S_LOCKUP) != 0;

    uint32_t pc = 0;
    ret = swd_read_word(DFSR_ADDR, &stop.dfsr);
    ret = ret ?: swd_read_core_register(CORE_REG_PC, &pc);
    stop.pc = pc;
    if (ret != ESP_OK) {
        return link_error(ret);
    }

    if ((stop.dfsr & DFSR_BKPT) != 0 && pc < FPB_CODE_LIMIT) {
        const uint32_t hit = fpb_comparator(pc);
        for (uint32_t i = 0; i < comparator_count; i++) {
            if (comparators[i] == hit) {
                stop.comparator_match = true;
                stop.comparator_index = i;
                break;
            }
        }
    }

    if (run_to_index >= 0) {
        comparators[run_to_index] = 0;
        ret = swd_write_word(FP_COMP_ADDR + run_to_index * 4, 0);
        run_to_index = -1;
    }
    return link_error(ret);
}

esp_err_t sidp_swd_backend::halt(uint32_t timeout_ms, sidp::stop_detect_t &stop)
{
    auto ret = poll_halted(stop);
    if (ret != ESP_OK || stop.halted) {
        return ret;
    }

    ret = write_control(C_HALT);
    if (ret != ESP_OK) {
        return link_error(ret);
    }

    const int64_t deadline_us = esp_timer_get_time() + timeout_ms * 1000LL;
    while (esp_timer_get_time() < deadline_us) {
        ret = poll_halted(stop);
        if (ret != ESP_OK || stop.halted) {
            return ret;
        }
        vTaskDelay(1);
    }

    return ESP_ERR_TIMEOUT;
}

esp_err_t sidp_swd_backend::resume(sidp::run_action_t action, uint64_t address)
{
    const bool step = action == sidp::RUN_SINGLE_STEP;
    auto ret = action == sidp::RUN_TO_ADDRESS ? install_run_to(address) : ESP_OK;
    ret = ret ?: swd_write_word(DFSR_ADDR, DFSR_CLEAR_ALL);

    // Mask interrupts so a step executes the next instruction instead of an
    // interrupt handler. The mask may only change while the core is halted.
    mask_bits = step ? C_MASKINTS : 0;
    ret = ret ?: write_control(C_HALT);
    ret = ret ?: write_control(step ? C_STEP : 0);
    return link_error(ret);
}

bool sidp_swd_backend::is_valid_reg(uint16_t id) const
{
    if (id == sidp::ARM_REG_BASEPRI || id == sidp::ARM_REG_FAULTMASK) {
        return profile != sidp::PROFILE_ARMV6M;
    }
    if (id >= sidp::ARM_REG_S0 && id <= sidp::ARM_REG_FPSCR) {
        return has_fpu;
    }
    return id <= sidp::ARM_REG_CONTROL;
}

esp_err_t sidp_swd_backend::read_reg(uint16_t id, uint32_t *value)
{
    if (!is_special_reg(id)) {
        return swd_read_core_register(core_reg_selector(id), value);
    }

    uint32_t special = 0;
    auto ret = swd_read_core_register(CORE_REG_SPECIAL, &special);
    *value = (special >> special_reg_shift(id)) & 0xFF;
    return ret;
}

esp_err_t sidp_swd_backend::append_reg(uint16_t id, std::span<uint8_t> out_blob, size_t &out_size)
{
    if (out_blob.size() - out_size < REG_ENTRY_SIZE) {
        return ESP_ERR_INVALID_SIZE;
    }

    uint32_t value = 0;
    auto ret = read_reg(id, &value);
    if (ret != ESP_OK && ret != ESP_ERR_TIMEOUT) {
        return link_error(ret);
    }

    // A register that never became ready is reported as unavailable, not as a failure.
    auto *entry = reinterpret_cast<sidp::register_value_t *>(out_blob.data() + out_size);
    entry->register_id = static_cast<sidp::register_id_t>(id);
    entry->value_size = sizeof(value);
    entry->flags = ret == ESP_OK ? sidp::REGISTER_VALUE_FLAG_NONE : sidp::REGISTER_VALUE_FLAG_UNAVAILABLE;
    memcpy(entry->value, &value, sizeof(value));
    out_size += REG_ENTRY_SIZE;
    return ESP_OK;
}

esp_err_t sidp_swd_backend::read_regs(std::span<const sidp::register_id_t> ids, std::span<uint8_t> out_blob, size_t &out_size)
{
    out_size = 0;

    if (ids.empty()) {
        for (uint16_t id = 0; id <= sidp::ARM_REG_FPSCR; id++) {
            if (is_valid_reg(id)) {
                auto ret = append_reg(id, out_blob, out_size);
                if (ret != ESP_OK) {
                    return ret;
                }
            }
        }
        return ESP_OK;
    }

    uint32_t seen[REG_SEEN_WORDS] = {};
    for (auto id : ids) {
        if (!is_valid_reg(id) || mark_seen(seen, id)) {
            return ESP_ERR_INVALID_ARG;
        }
    }

    for (auto id : ids) {
        auto ret = append_reg(id, out_blob, out_size);
        if (ret != ESP_OK) {
            return ret;
        }
    }
    return ESP_OK;
}

esp_err_t sidp_swd_backend::write_reg(uint16_t id, uint32_t value)
{
    if (!is_special_reg(id)) {
        return swd_write_core_register(core_reg_selector(id), value);
    }

    // The special registers share one core register, so keep the other three.
    uint32_t special = 0;
    auto ret = swd_read_core_register(CORE_REG_SPECIAL, &special);
    const uint32_t shift = special_reg_shift(id);
    special = (special & ~(0xFFu << shift)) | ((value & 0xFF) << shift);
    return ret ?: swd_write_core_register(CORE_REG_SPECIAL, special);
}

esp_err_t sidp_swd_backend::write_regs(const uint8_t *data, size_t size)
{
    if (size % REG_ENTRY_SIZE != 0) {
        return ESP_ERR_INVALID_ARG;
    }

    // Reject the whole request before writing anything.
    uint32_t seen[REG_SEEN_WORDS] = {};
    for (size_t offset = 0; offset < size; offset += REG_ENTRY_SIZE) {
        const auto *entry = reinterpret_cast<const sidp::register_value_t *>(data + offset);
        if (entry->value_size != sizeof(uint32_t) || entry->flags != sidp::REGISTER_VALUE_FLAG_NONE ||
            !is_valid_reg(entry->register_id) || mark_seen(seen, entry->register_id)) {
            return ESP_ERR_INVALID_ARG;
        }
    }

    for (size_t offset = 0; offset < size; offset += REG_ENTRY_SIZE) {
        const auto *entry = reinterpret_cast<const sidp::register_value_t *>(data + offset);
        uint32_t value = 0;
        memcpy(&value, entry->value, sizeof(value));
        auto ret = write_reg(entry->register_id, value);
        if (ret != ESP_OK) {
            return link_error(ret);
        }
    }
    return ESP_OK;
}

esp_err_t sidp_swd_backend::read_mem(uint64_t address, uint8_t *out, size_t size, sidp::memory_access_width_t width)
{
    auto addr = static_cast<uint32_t>(address);
    auto ret = ESP_OK;

    switch (width) {
    case sidp::MEM_WIDTH_DEFAULT:
    case sidp::MEM_WIDTH_32:
        // The session has checked alignment, so a MEM_WIDTH_32 read is all word accesses.
        ret = swd_read_memory(addr, out, size);
        break;
    case sidp::MEM_WIDTH_8:
        for (size_t i = 0; i < size && ret == ESP_OK; i++) {
            ret = swd_read_byte(addr + i, out + i);
        }
        break;
    default:
        return ESP_ERR_NOT_SUPPORTED; // swd_host has no halfword access
    }

    return memory_error(ret);
}

esp_err_t sidp_swd_backend::write_mem(uint64_t address, const uint8_t *data, size_t size, sidp::memory_access_width_t width)
{
    auto addr = static_cast<uint32_t>(address);
    auto ret = ESP_OK;

    switch (width) {
    case sidp::MEM_WIDTH_DEFAULT:
    case sidp::MEM_WIDTH_32:
        // The session has checked alignment, so a MEM_WIDTH_32 write is all word accesses.
        ret = swd_write_memory(addr, const_cast<uint8_t *>(data), size);
        break;
    case sidp::MEM_WIDTH_8:
        for (size_t i = 0; i < size && ret == ESP_OK; i++) {
            ret = swd_write_byte(addr + i, data[i]);
        }
        break;
    default:
        return ESP_ERR_NOT_SUPPORTED; // swd_host has no halfword access
    }

    return memory_error(ret);
}

esp_err_t sidp_swd_backend::setup_comparators()
{
    uint32_t fp_ctrl = 0;
    auto ret = swd_read_word(FP_CTRL_ADDR, &fp_ctrl);
    if (ret != ESP_OK) {
        return ret;
    }

    const uint32_t count = ((fp_ctrl >> 8) & 0x70) | ((fp_ctrl >> 4) & 0x0F);
    comparator_count = count < MAX_COMPARATORS ? count : MAX_COMPARATORS;

    // Another debugger may have left breakpoints behind.
    ret = clear_comparators();
    if (ret == ESP_OK && comparator_count != 0) {
        ret = swd_write_word(FP_CTRL_ADDR, FP_CTRL_KEY | FP_CTRL_ENABLE);
    }
    return ret;
}

esp_err_t sidp_swd_backend::install_run_to(uint32_t address)
{
    if (address >= FPB_CODE_LIMIT) {
        return ESP_ERR_NOT_SUPPORTED;
    }

    const uint32_t value = fpb_comparator(address);
    int32_t free_index = -1;
    for (uint32_t i = 0; i < comparator_count; i++) {
        if (comparators[i] == value) {
            return ESP_OK; // Reuse the breakpoint already at this address
        }
        if (free_index < 0 && comparators[i] == 0) {
            free_index = i;
        }
    }
    if (free_index < 0) {
        return ESP_ERR_NOT_FOUND;
    }

    comparators[free_index] = value;
    run_to_index = free_index;
    return swd_write_word(FP_COMP_ADDR + free_index * 4, value);
}

esp_err_t sidp_swd_backend::clear_comparators()
{
    auto ret = ESP_OK;
    run_to_index = -1;
    for (uint32_t i = 0; i < comparator_count; i++) {
        comparators[i] = 0;
        ret = ret ?: swd_write_word(FP_COMP_ADDR + i * 4, 0);
    }
    return ret;
}

esp_err_t sidp_swd_backend::disable_comparators()
{
    if (comparator_count == 0) {
        return ESP_OK;
    }

    auto ret = clear_comparators();
    return ret ?: swd_write_word(FP_CTRL_ADDR, FP_CTRL_KEY);
}

esp_err_t sidp_swd_backend::apply_breakpoints(std::span<const sidp::bp_entry_t> entries)
{
    if (entries.size() > comparator_count) {
        return ESP_ERR_NOT_SUPPORTED;
    }

    for (const auto &entry : entries) {
        if (entry.kind != sidp::BREAKPOINT_HARDWARE || entry.address >= FPB_CODE_LIMIT || (entry.address & 1) != 0) {
            return ESP_ERR_INVALID_ARG;
        }
    }

    auto ret = ESP_OK;
    run_to_index = -1;
    for (uint32_t i = 0; i < comparator_count; i++) {
        comparators[i] = i < entries.size() ? fpb_comparator(entries[i].address) : 0;
        ret = ret ?: swd_write_word(FP_COMP_ADDR + i * 4, comparators[i]);
    }

    if (ret != ESP_OK) {
        // The contract forbids leaving a partial set installed.
        clear_comparators();
    }
    return link_error(ret);
}

esp_err_t sidp_swd_backend::apply_watchpoints(std::span<const sidp::wp_entry_t> entries)
{
    // Watchpoints are not advertised; only the session's clear must succeed.
    return entries.empty() ? ESP_OK : ESP_ERR_NOT_SUPPORTED;
}

esp_err_t sidp_swd_backend::set_vector_catch(sidp::vector_catch_t mask)
{
    // Vector catch is not advertised; only the session's clear must succeed.
    return mask == sidp::VECTOR_CATCH_NONE ? ESP_OK : ESP_ERR_NOT_SUPPORTED;
}
