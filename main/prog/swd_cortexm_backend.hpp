#pragma once

#include "target_backend.hpp"

/**
 * Cortex-M implementation of target_backend on top of swd_prog.
 * All erase/program/verify semantics are inherited from the existing
 * SWD flash-algorithm machinery.
 */
class swd_cortexm_backend : public target_backend
{
public:
    static swd_cortexm_backend *instance()
    {
        static swd_cortexm_backend _instance;
        return &_instance;
    }

    swd_cortexm_backend(swd_cortexm_backend const &) = delete;
    void operator=(swd_cortexm_backend const &) = delete;

    const char *name() override
    {
        return "cortex-m (SWD)";
    }

    esp_err_t begin_session() override;
    esp_err_t detect() override;
    esp_err_t erase() override;
    esp_err_t program(uint32_t *written_len) override;
    esp_err_t verify(uint32_t written_len) override;
    esp_err_t self_test(const si::config::test_item &item, uint32_t *func_return_val) override;
    esp_err_t release_transport() override;
    esp_err_t reset_target() override;
    esp_err_t reinit_debug() override;
    esp_err_t halt_target() override;
    esp_err_t wait_halt() override;
    esp_err_t read_mem32(uint32_t addr, uint32_t *val) override;
    esp_err_t write_mem32(uint32_t addr, uint32_t val) override;

private:
    swd_cortexm_backend() = default;
};
