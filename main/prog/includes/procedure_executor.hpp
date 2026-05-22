#pragma once

#include <cstdint>
#include <esp_err.h>
#include <vector>

class procedure_executor
{
public:
    enum step_type : uint32_t
    {
        READ_32 = 0,
        WRITE_32 = 1,
        READ_BLOB = 2,
        WRITE_BLOB = 3,
        READ_MOD_WRITE_32 = 4,
        POLL_32 = 5,
        DELAY_MS = 6,
        SWD_REINIT = 7,
        SWD_RESET_TARGET = 8,
        SWD_HALT_TARGET = 9,
        SWD_WAIT_HALT = 10,
    };

    struct step_rw32
    {
        uint32_t addr;
        uint32_t data;
    };

    struct step_rwblob
    {
        uint32_t addr;
        uint8_t *buf;
        size_t buf_len;
    };

    struct step_rmw32
    {
        uint32_t addr;
        uint32_t mask; // The "AND" mask
        uint32_t data; // The data to be "OR'ed"
    };

    struct step_poll32
    {
        // It's like while (!timeout--) { if (((*(volatile uint32_t *)(uintptr_t)addr) & mask) == expected) { break; } else { continue; }  }
        uint32_t addr;
        uint32_t mask; // The mask to be waited
        uint32_t expected; // Expected bit
        uint32_t timeout_ms; // Timeout
    };

    struct step_delay_ms
    {
        uint32_t delay_ms;
    };

    struct step
    {
        step_type type;
        union {
            step_rw32 rw32;
            step_rwblob rwblob;
            step_rmw32 rmw32;
            step_poll32 poll32;
            step_delay_ms delay_ms;
        } op;
    };

public:
    procedure_executor() : steps({}) {}
    explicit procedure_executor(std::vector<procedure_executor::step> const &_steps);

    esp_err_t load_yaml(const char *path);
    esp_err_t execute();

private:
    esp_err_t exec_rw32(procedure_executor::step *curr_step);
    esp_err_t exec_rwblob(procedure_executor::step *curr_step);
    esp_err_t exec_rmw32(procedure_executor::step *curr_step);
    esp_err_t exec_poll32(procedure_executor::step *curr_step);
    esp_err_t exec_delay_ms(procedure_executor::step *curr_step);
    esp_err_t exec_swd_reinit(procedure_executor::step *curr_step);
    esp_err_t exec_swd_reset(procedure_executor::step *curr_step);
    esp_err_t exec_swd_halt(procedure_executor::step *curr_step);
    esp_err_t exec_swd_wait_halt(procedure_executor::step *curr_step);

private:
    std::vector<procedure_executor::step> steps;
    static constexpr char TAG[] = "procedure_exec";
};
