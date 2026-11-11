#include "sidp_swd_backend.hpp"

#include <cstring>

#include <esp_log.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <swd_host.h>

static const constexpr uint32_t CORE_REG_PC = 15;
static const constexpr uint32_t CORE_REG_SPECIAL = 20; // CONTROL, FAULTMASK, BASEPRI and PRIMASK, one byte each
static const constexpr uint32_t CORE_REG_FPSCR = 33;

static const constexpr size_t REG_ENTRY_SIZE = sizeof(sidp::register_value_t) + sizeof(uint32_t);
static const constexpr size_t REG_SEEN_WORDS = sidp::ARM_REG_FPSCR / 32 + 1;

static const constexpr uint32_t CPUID_ADDR = 0xE000ED00;
static const constexpr uint32_t DFSR_ADDR = 0xE000ED30;
static const constexpr uint32_t DHCSR_ADDR = 0xE000EDF0;
static const constexpr uint32_t MVFR0_ADDR = 0xE000EF40;

static const constexpr uint32_t CPUID_IMPLEMENTER_ARM = 0x41;
static const constexpr uint32_t CPUID_PART_M0 = 0xC20;
static const constexpr uint32_t CPUID_PART_M0P = 0xC60;
static const constexpr uint32_t CPUID_PART_M3 = 0xC23;
static const constexpr uint32_t CPUID_PART_M4 = 0xC24;

static const constexpr uint32_t DFSR_CLEAR_ALL = 0x1F;

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

// Records id in the seen set. Returns true if it was already there.
static bool mark_seen(uint32_t *seen, uint16_t id)
{
    const uint32_t bit = 1u << (id % 32);
    const bool duplicate = (seen[id / 32] & bit) != 0;
    seen[id / 32] |= bit;
    return duplicate;
}

esp_err_t sidp_swd_backend::set_target(uint32_t id, const sidp::memory_region_t *regions, size_t count)
{
    if (regions == nullptr || count == 0 || count > MAX_REGIONS) {
        return ESP_ERR_INVALID_ARG;
    }

    target_id = id;
    region_count = count;
    memcpy(memory_regions, regions, count * sizeof(sidp::memory_region_t));
    return ESP_OK;
}

esp_err_t sidp_swd_backend::attach(const sidp::attach_params_t &params, sidp::attach_info_t &info)
{
    if (connected || region_count == 0) {
        ESP_LOGE(TAG, "attach: already connected or no target definition set");
        return ESP_ERR_INVALID_STATE;
    }

    if (params.connect_under_reset) {
        return ESP_ERR_NOT_SUPPORTED;
    }

    auto ret = swd_init_debug(pdMS_TO_TICKS(SWD_LOCK_TIMEOUT_MS));
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "attach: cannot connect: %s", esp_err_to_name(ret));
        return link_error(ret);
    }

    ret = read_target_info(info);
    if (ret != ESP_OK) {
        swd_off();
        return ret;
    }

    connected = true;
    return ESP_OK;
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
    ret = swd_write_word(DHCSR_ADDR, DBGKEY | C_DEBUGEN | mask_bits | (dhcsr & C_HALT));
    if (ret == ESP_OK && (dhcsr & S_HALT) == 0) {
        // Old stop causes must not be mistaken for the next stop.
        ret = swd_write_word(DFSR_ADDR, DFSR_CLEAR_ALL);
    }
    if (ret != ESP_OK) {
        return link_error(ret);
    }

    uint32_t capabilities = sidp::CAP_STOP_SNAPSHOT;

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
    info.memory_regions = {reinterpret_cast<const uint8_t *>(memory_regions), region_count * sizeof(sidp::memory_region_t)};
    return ESP_OK;
}

esp_err_t sidp_swd_backend::detach(sidp::detach_action_t action)
{
    if (!connected) {
        return ESP_OK;
    }

    auto ret = ESP_OK;
    if (action == sidp::DETACH_RESUME) {
        // The interrupt mask may only change while halted. Clearing C_DEBUGEN then resumes.
        mask_bits = 0;
        ret = write_control(C_HALT);
        ret = ret ?: swd_write_word(DHCSR_ADDR, DBGKEY);
    }
    if (ret != ESP_OK) {
        return link_error(ret);
    }

    connected = false;
    return swd_off();
}

esp_err_t sidp_swd_backend::reset(sidp::reset_kind_t kind, bool halt_after, sidp::stop_detect_t &stop)
{
    return ESP_ERR_NOT_SUPPORTED;
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
    return ESP_ERR_NOT_SUPPORTED;
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

esp_err_t sidp_swd_backend::apply_breakpoints(std::span<const sidp::bp_entry_t> entries)
{
    return entries.empty() ? ESP_OK : ESP_ERR_NOT_SUPPORTED;
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
