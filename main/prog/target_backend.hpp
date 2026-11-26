#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>

#include <esp_err.h>

#include "config/target_config.hpp"

/**
 * Family-agnostic programming backend.
 *
 * offline_flasher drives the programming state machine against this
 * interface; it does not know or care whether the target is an ARM
 * Cortex-M reached over SWD or an Espressif chip reached over the UART
 * ROM bootloader. procedure_executor also uses it so that YAML procedures
 * (pre_prog/post_prog) can toggle reset etc. on either family.
 *
 * Implementation rules:
 *  - allocate working buffers once in detect()/begin_session(), never in
 *    the per-page hot path;
 *  - all methods are idempotent enough to be retried by the FSM.
 */
class target_backend
{
public:
    virtual ~target_backend() = default;

    virtual const char *name() = 0;

    /**
     * Bring the transport up and put the target into a known state
     * (e.g. cycle NRST with SWD pins owned, or idle the UART pins).
     * Called before pre_prog and post_prog procedures.
     */
    virtual esp_err_t begin_session() = 0;

    /** Detect/connect the target. Populates any backend-side state. */
    virtual esp_err_t detect() = 0;

    /** Erase what needs erasing before programming. */
    virtual esp_err_t erase() = 0;

    /** Program all configured images. @param written_len total bytes programmed. */
    virtual esp_err_t program(uint32_t *written_len) = 0;

    /** Verify what program() wrote. */
    virtual esp_err_t verify(uint32_t written_len) = 0;

    /** Run one self test. ESP_ERR_NOT_SUPPORTED = family has no self tests. */
    virtual esp_err_t self_test(const si::config::test_item &item, uint32_t *func_return_val) = 0;

    /**
     * Release the transport so the target can run its fresh firmware
     * (tri-state SWD, deinit UART, final reset).
     */
    virtual esp_err_t release_transport() = 0;

    // ----- operations used by YAML procedures (pre_prog/post_prog) -----

    /** Toggle the target reset line. */
    virtual esp_err_t reset_target() = 0;

    /** Re-initialise the debug connection after a target reset. */
    virtual esp_err_t reinit_debug() = 0;

    /** Halt the target CPU. */
    virtual esp_err_t halt_target() = 0;

    /** Wait until the target CPU is halted. */
    virtual esp_err_t wait_halt() = 0;

    /** Read one 32-bit word from target memory space. */
    virtual esp_err_t read_mem32(uint32_t addr, uint32_t *val) = 0;

    /** Write one 32-bit word to target memory space. */
    virtual esp_err_t write_mem32(uint32_t addr, uint32_t val) = 0;


    using progress_fn = void (*)(uint32_t done, uint32_t total);
    progress_fn on_progress = nullptr; // done/total bytes of program()

    virtual esp_err_t read_mem(uint32_t addr, uint8_t *buf, size_t len)
    {
        for (size_t i = 0; i < len; i += 4) {
            uint32_t w = 0;
            esp_err_t ret = ((addr | len) & 3) ? ESP_ERR_INVALID_ARG : read_mem32(addr + i, &w);
            if (ret != ESP_OK) {
                return ret;
            }
            memcpy(buf + i, &w, 4);
        }
        return ESP_OK;
    }

    virtual esp_err_t write_mem(uint32_t addr, const uint8_t *buf, size_t len)
    {
        for (size_t i = 0; i < len; i += 4) {
            uint32_t w = 0;
            memcpy(&w, buf + i, 4);
            esp_err_t ret = ((addr | len) & 3) ? ESP_ERR_INVALID_ARG : write_mem32(addr + i, w);
            if (ret != ESP_OK) {
                return ret;
            }
        }
        return ESP_OK;
    }

    virtual esp_err_t flash_read(uint32_t addr, uint8_t *buf, size_t len) { return ESP_ERR_NOT_SUPPORTED; }
    virtual esp_err_t flash_write(uint32_t addr, const uint8_t *buf, size_t len) { return ESP_ERR_NOT_SUPPORTED; }
    virtual esp_err_t flash_erase(uint32_t addr, uint32_t count) { return ESP_ERR_NOT_SUPPORTED; }

    enum class flash_op { read, write, erase };
    /** A flash_read/write/erase call's arguments, checked without touching the
     *  target (`len`: bytes, or for erase a count of sectors): ESP_ERR_INVALID_ARG with the reason in `why` when they're wrong.
     *  Any other error is the call's own (no flash, not connected yet). */
    virtual esp_err_t flash_check(flash_op op, uint32_t addr, size_t len, char *why, size_t why_len)
    {
        return ESP_ERR_NOT_SUPPORTED;
    }

    virtual esp_err_t call(uint32_t addr, const uint32_t args[4], uint32_t stack_top, uint32_t timeout_ms,
                           bool (*cancelled)(), uint32_t *r0)
    {
        return ESP_ERR_NOT_SUPPORTED;
    }

    /** What a run leaves the target doing when it ends. */
    enum class run_end {
        reset,  // debug state cleared and the target reset: the firmware boots
        resume, // debug state cleared: the core goes on from where it is
        as_is,  // only the transport is released
    };

    /**
     * End a run: leave the target as `how` says and release the transport.
     * Families with one way to end (ESP32: reset into the application) ignore `how`.
     */
    virtual esp_err_t end_run(run_end how) { return release_transport(); }

    virtual esp_err_t resume_target() { return ESP_ERR_NOT_SUPPORTED; }
    virtual esp_err_t read_core_reg(uint32_t id, uint32_t *val) { return ESP_ERR_NOT_SUPPORTED; } // ESP_ERR_INVALID_STATE: running
    virtual esp_err_t write_core_reg(uint32_t id, uint32_t val) { return ESP_ERR_NOT_SUPPORTED; }
};
