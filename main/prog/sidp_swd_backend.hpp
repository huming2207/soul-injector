#pragma once

#include <esp_pm.h>
#include <sidp_backend.hpp>

/**
 * Cortex-M debug backend for the SIDP session, on top of swd_host.
 *
 * Every call must come from the one pinned task that owns the debug session,
 * because swd_host ties the SWD bus to the task that started it. No heap is
 * used. See sidp_backend.hpp for the contract each method follows.
 */
class sidp_swd_backend final : public sidp::target_backend_t
{
public:
    static sidp_swd_backend *instance()
    {
        static sidp_swd_backend _instance;
        return &_instance;
    }

    /**
     * Sets the target definition attach() reports. The regions are copied.
     * @param id Identifies the selected target definition
     * @return ESP_ERR_INVALID_ARG if there are no regions or too many
     */
    esp_err_t set_target(uint32_t id, const sidp::memory_region_t *regions, size_t count);

    esp_err_t attach(const sidp::attach_params_t &params, sidp::attach_info_t &info) override;
    esp_err_t detach(sidp::detach_action_t action) override;
    esp_err_t reset(sidp::reset_kind_t kind, bool halt_after, sidp::stop_detect_t &stop) override;
    esp_err_t poll_halted(sidp::stop_detect_t &stop) override;
    esp_err_t halt(uint32_t timeout_ms, sidp::stop_detect_t &stop) override;
    esp_err_t resume(sidp::run_action_t action, uint64_t address) override;
    esp_err_t read_regs(std::span<const sidp::register_id_t> ids, std::span<uint8_t> out_blob, size_t &out_size) override;
    esp_err_t write_regs(const uint8_t *data, size_t size) override;
    esp_err_t read_mem(uint64_t address, uint8_t *out, size_t size, sidp::memory_access_width_t width) override;
    esp_err_t write_mem(uint64_t address, const uint8_t *data, size_t size, sidp::memory_access_width_t width) override;
    esp_err_t apply_breakpoints(std::span<const sidp::bp_entry_t> entries) override;
    esp_err_t apply_watchpoints(std::span<const sidp::wp_entry_t> entries) override;
    esp_err_t set_vector_catch(sidp::vector_catch_t mask) override;

private:
    sidp_swd_backend() = default;

    esp_err_t claim_target();
    void release_target();
    esp_err_t read_target_info(sidp::attach_info_t &info);
    esp_err_t write_control(uint32_t bits);
    bool is_valid_reg(uint16_t id) const;
    esp_err_t read_reg(uint16_t id, uint32_t *value);
    esp_err_t append_reg(uint16_t id, std::span<uint8_t> out_blob, size_t &out_size);
    esp_err_t write_reg(uint16_t id, uint32_t value);
    esp_err_t setup_comparators();
    esp_err_t clear_comparators();
    esp_err_t disable_comparators();
    esp_err_t install_run_to(uint32_t address);
    esp_err_t set_reset_catch(bool enable);
    esp_err_t finish_reset(bool halt_after, sidp::stop_detect_t &stop);
    void pulse_nrst();
    esp_err_t connect_under_reset(bool halt_after);

private:
    static const constexpr char TAG[] = "sidp_swd";
    static const constexpr size_t MAX_REGIONS = 16;
    static const constexpr uint32_t MAX_COMPARATORS = 8;
    static const constexpr uint32_t SWD_LOCK_TIMEOUT_MS = 1000;
    static const constexpr uint32_t RESET_PULSE_MS = 20;
    static const constexpr uint32_t RESET_SETTLE_MS = 20;
    static const constexpr uint32_t RESET_TIMEOUT_MS = 1000;

    bool connected = false;
    esp_pm_lock_handle_t pm_lock = nullptr;
    uint32_t mask_bits = 0;
    sidp::target_profile_t profile = sidp::PROFILE_ARMV6M;
    bool has_fpu = false;
    uint32_t comparator_count = 0;
    uint32_t comparators[MAX_COMPARATORS] = {};
    int32_t run_to_index = -1;
    uint32_t target_id = 0;
    size_t region_count = 0;
    sidp::memory_region_t memory_regions[MAX_REGIONS] = {};
};
