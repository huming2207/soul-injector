#pragma once

#include <cstdint>
#include <cstring>
#include <variant>

#include <esp_err.h>
#include <esp_log.h>
#include <ryml.hpp>

#include "config/yaml_doc.hpp"
#include "target_backend.hpp"

/**
 * YAML-driven pre/post programming procedure executor.
 *
 * Each step is a trivially-copyable value type stored in a fixed-capacity
 * in-object array: no heap allocation at parse or execute time. Steps are
 * dispatched through std::visit on a std::variant, and target operations go
 * through target_backend so the same YAML procedures work for every target
 * family (SWD today, ESP32 serial next).
 *
 * YAML shape (unchanged from previous releases):
 *
 *   steps:
 *     - type: WRITE_32
 *       addr: 0x40000000
 *       data: 0x00000001
 */
class procedure_executor
{
public:
    // NOTE: no default member initialiser here. An NSDMI would make the step
    // structs non-trivially default constructible before the enclosing class
    // is complete, which breaks std::variant's default constructor (diagnosed
    // by both GCC and clangd). Every construction path value-initialises:
    // `read32_step s = {};` zeroes all fields including ignore_error.
    struct step_common {
        bool ignore_error;
    };

    struct read32_step : step_common {
        uint32_t addr;
    };

    struct write32_step : step_common {
        uint32_t addr;
        uint32_t data;
    };

    struct read_mod_write32_step : step_common {
        uint32_t addr;
        uint32_t mask; // AND mask
        uint32_t data; // OR data
    };

    struct poll32_step : step_common {
        uint32_t addr;
        uint32_t mask;
        uint32_t expected;
        uint32_t timeout_ms;
    };

    struct delay_ms_step : step_common {
        uint32_t delay_ms;
    };

    struct swd_reinit_step : step_common {
    };

    struct reset_target_step : step_common {
    };

    struct halt_target_step : step_common {
    };

    struct wait_halt_step : step_common {
    };

    using step = std::variant<
        read32_step, write32_step, read_mod_write32_step, poll32_step, delay_ms_step, swd_reinit_step, reset_target_step, halt_target_step,
        wait_halt_step>;

    static constexpr size_t MAX_STEPS = 96;

public:
    procedure_executor() = default;

    /** Parse a procedure YAML. Replaces any previously loaded steps. */
    esp_err_t load_yaml(const char *path);

    /** Execute all steps against @p backend; aborts on the first unignored failure. */
    esp_err_t execute(target_backend &backend);

    void clear()
    {
        step_count = 0;
    }

    size_t size() const
    {
        return step_count;
    }

private:
    static esp_err_t parse_step(ryml::ConstNodeRef node, size_t idx, step &out);
    static esp_err_t exec_one(const step &s, target_backend &backend);

    // Uninitialised until assigned; reads are guarded by step_count.
    step steps[MAX_STEPS];
    size_t step_count = 0;

    static constexpr char TAG[] = "procedure_exec";
};
