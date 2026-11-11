#include "sidp_swd_backend.hpp"

#include <cstring>

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
    return ESP_ERR_NOT_SUPPORTED;
}

esp_err_t sidp_swd_backend::detach(sidp::detach_action_t action)
{
    return ESP_ERR_NOT_SUPPORTED;
}

esp_err_t sidp_swd_backend::reset(sidp::reset_kind_t kind, bool halt_after, sidp::stop_detect_t &stop)
{
    return ESP_ERR_NOT_SUPPORTED;
}

esp_err_t sidp_swd_backend::poll_halted(sidp::stop_detect_t &stop)
{
    return ESP_ERR_NOT_SUPPORTED;
}

esp_err_t sidp_swd_backend::halt(uint32_t timeout_ms, sidp::stop_detect_t &stop)
{
    return ESP_ERR_NOT_SUPPORTED;
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
    return ESP_ERR_NOT_SUPPORTED;
}

esp_err_t sidp_swd_backend::write_mem(uint64_t address, const uint8_t *data, size_t size, sidp::memory_access_width_t width)
{
    return ESP_ERR_NOT_SUPPORTED;
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
