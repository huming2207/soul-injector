#pragma once

#include <cstdint>

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
};
