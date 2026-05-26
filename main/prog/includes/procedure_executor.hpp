#pragma once

#include <cstdint>
#include <vector>
#include <cstring>

#include <esp_err.h>
#include <esp_log.h>
#include <ryml.hpp>


class procedure_executor
{
public:
    enum step_type : int32_t
    {
        UNKNOWN_TYPE = -1,
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
        bool ignore_error;
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
    void clear();

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
    step_type string_to_type(ryml::csubstr type_str)
    {
        if (type_str == "READ_32") return READ_32;
        if (type_str == "WRITE_32") return WRITE_32;
        if (type_str == "READ_BLOB") return READ_BLOB;
        if (type_str == "WRITE_BLOB") return WRITE_BLOB;
        if (type_str == "READ_MOD_WRITE_32") return READ_MOD_WRITE_32;
        if (type_str == "POLL_32") return POLL_32;
        if (type_str == "DELAY_MS") return DELAY_MS;
        if (type_str == "SWD_REINIT") return SWD_REINIT;
        if (type_str == "SWD_RESET_TARGET") return SWD_RESET_TARGET;
        if (type_str == "SWD_HALT_TARGET") return SWD_HALT_TARGET;
        if (type_str == "SWD_WAIT_HALT") return SWD_WAIT_HALT;

        ESP_LOGW(TAG, "load_yml: skipping unknown type: %.*s", type_str.size(), type_str.data());
        return UNKNOWN_TYPE; // Default fallback
    }

    uint32_t parse_number(const ryml::ConstNodeRef& node)
    {
        if (node.invalid() || node.is_seed()) return 0;
        ryml::csubstr val = node.val();
        char buf[32];
        size_t len = val.len < sizeof(buf) - 1 ? val.len : sizeof(buf) - 1;
        std::memcpy(buf, val.str, len);
        buf[len] = '\0';
        return std::strtoul(buf, nullptr, 0);
    }

private:
    std::vector<procedure_executor::step> steps;
    static constexpr char TAG[] = "procedure_exec";
};
