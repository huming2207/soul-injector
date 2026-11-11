#include "sidp_swd_backend.hpp"

#include <cstring>

#include <esp_log.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <swd_host.h>

static const constexpr uint32_t CORE_REG_PC = 15;

static const constexpr uint32_t CPUID_ADDR = 0xE000ED00;
static const constexpr uint32_t DFSR_ADDR = 0xE000ED30;
static const constexpr uint32_t DHCSR_ADDR = 0xE000EDF0;

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

    uint32_t capabilities = 0;

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

esp_err_t sidp_swd_backend::read_regs(std::span<const sidp::register_id_t> ids, std::span<uint8_t> out_blob, size_t &out_size)
{
    return ESP_ERR_NOT_SUPPORTED;
}

esp_err_t sidp_swd_backend::write_regs(const uint8_t *data, size_t size)
{
    return ESP_ERR_NOT_SUPPORTED;
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
