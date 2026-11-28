#pragma once

#include <esp_err.h>
#include <swd_host.h>

#include "config/target_config.hpp"
#include "led_ctrl.hpp"

namespace swd_def
{
    // Dedicated GPIO belongs to the core that sets it up, so every task using SWD runs here.
    static const constexpr int TASK_CORE = 1;

    enum state : uint8_t {
        UNKNOWN = 0,
        INITIALISED = 1,
        FLASH_ALG_LOADED = 2,
        FLASH_ALG_INITED = 3,
        FLASH_ALG_RUNNING = 4,
        FLASH_ALG_UNINITED = 5,
    };

    enum init_mode : uint8_t {
        ERASE = 1,
        PROGRAM = 2,
        VERIFY = 3,
    };
} // namespace swd_def

/**
 * SWD flash programmer for ARM Cortex-M targets, driven by the typed
 * target_config parsed from target.yaml.
 */
class swd_prog
{
public:
    static swd_prog *instance()
    {
        static swd_prog _instance;
        return &_instance;
    }

    swd_prog(swd_prog const &) = delete;
    void operator=(swd_prog const &) = delete;

private:
    swd_def::state state = swd_def::UNKNOWN;
    program_syscall_t syscall = {};
    uint32_t code_start = 0;
    uint32_t code_end = 0;
    uint32_t stack_bottom = 0; // Offset of stack bottom
    uint32_t stack_top = 0;    // Offset of stack top
    uint32_t stack_canary = 0; // Random 32-bit word generated on every init
    uint32_t ram_start_addr = 0;
    uint32_t ram_size = 0;
    uint32_t stack_size = 0;
    size_t algo_bin_len = 0;
    led_ctrl &led = led_ctrl::instance();
    uint8_t programmed_hash[32] = {}; // SHA256 for the stuff programmed?
    uint32_t layout_generation = 0;   // config the layout is for
    bool have_layout = false;

    // The algorithm load_flash_algorithm() wrote is still in target RAM, for
    // config `resident_generation`, and nothing since could have changed it:
    // the next flash operation may skip the reload (and its target reset)
    // once reuse_flash_algorithm() has checked it.
    bool algo_resident = false;
    uint32_t resident_generation = 0;

private:
    static const constexpr uint32_t halt_header = 0xBE00BE00; // Two breakpoint instructions.
                                                              // See https://github.com/probe-rs/probe-rs/pull/2883

private:
    swd_prog() = default;
    esp_err_t connect_and_halt();
    esp_err_t load_flash_algorithm();
    esp_err_t reuse_flash_algorithm();
    esp_err_t run_algo_init(swd_def::init_mode mode);
    esp_err_t run_algo_uninit(swd_def::init_mode mode);
    esp_err_t ensure_layout();
    static inline uint32_t next_multiple_of(uint32_t input, uint32_t of);
    inline esp_err_t perform_double_buffered_program(FILE *file, uint32_t len, uint32_t page_size, uint32_t pc_program_page, uint32_t addr_offset);
    inline esp_err_t perform_simple_program(FILE *file, uint32_t len, uint32_t page_size, uint32_t pc_program_page, uint32_t addr_offset);

    /** Access to the currently loaded flash algorithm config. */
    static const si::config::flash_algorithm &algo();

public:
    esp_err_t init(uint32_t _stack_size = 0x2000);
    esp_err_t erase_chip();
    esp_err_t erase_sector(uint32_t start_addr, uint32_t end_addr);
    esp_err_t program_page(const uint8_t *buf, size_t len, uint32_t start_addr = UINT32_MAX);
    esp_err_t program_file(const char *path, uint32_t *len_written = nullptr, uint32_t start_addr = UINT32_MAX);
    esp_err_t verify(const char *path, uint32_t start_addr = UINT32_MAX, size_t len = 0);
    esp_err_t self_test(uint32_t test_id, uint8_t *readout_buf, size_t readout_buf_len = 0, uint32_t *func_return_val = nullptr);

    enum class flash_access { read, write, erase };
    /**
     * Checks a flash access against the job's flash algorithm alone (no
     * target access): inside its flash range, a multiple of 4 bytes for a
     * write. For an erase `len` counts sectors of the job's sector map, the
     * first starting at `addr`. ESP_ERR_INVALID_ARG with the reason in
     * `why` when the arguments are wrong; ESP_ERR_INVALID_STATE when the job
     * has no flash range, page size or the algorithm function needed.
     */
    static esp_err_t check_flash_args(flash_access op, uint32_t addr, uint32_t len, char *why, size_t why_len);

    /** An FP register by DCRSR REGSEL (FPSCR, S0-S31): only there with an FPU. */
    static bool is_fp_reg(uint32_t id) { return id == 0x21 || (id >= 0x40 && id <= 0x5F); }
    /**
     * Whether the attached core has a single precision FPU (MVFR0). Without
     * one, DCRSR still answers for the FP registers, with values that mean
     * nothing. One SWD read: callers ask once per attach and keep the answer.
     */
    static bool read_has_fpu();

    /**
     * The flash algorithm stays loaded in target RAM between flash operations
     * of one job run or agent session. Each reuse first checks the core is
     * halted with no reset since and the halt header and stack canary are
     * unchanged, then writes the image again (as fast as reading it back, and
     * it may hold the algorithm's own variables). Callers drop it (forget_algorithm) when the
     * target may have run or changed it: a reset, a resume, a call, a debug
     * reconnect, a write over its RAM, and at the start and end of every run
     * or session. The next operation then reloads it, resetting the target.
     */
    void forget_algorithm()
    {
        algo_resident = false;
        state = swd_def::UNKNOWN; // or the next op would skip Init and call into RAM as it was
    }
    /**
     * For every DHCSR read outside the algorithm calls: reading clears the
     * sticky S_RESET_ST that reuse_flash_algorithm() relies on, so a reader
     * that sees a reset (or a running core) drops the algorithm itself.
     */
    void note_dhcsr(uint32_t dhcsr)
    {
        if ((dhcsr & S_RESET_ST) != 0 || (dhcsr & S_HALT) == 0) {
            forget_algorithm();
        }
    }
    /** Whether [addr, addr+len) touches the RAM the loaded algorithm needs kept: header, code, data, stack canary. */
    bool overlaps_algorithm(uint32_t addr, size_t len) const;

    /** Erases `count` sectors of the job's sector map, the first starting at `addr`. */
    esp_err_t erase_sectors(uint32_t addr, uint32_t count);
    esp_err_t write_range(uint32_t addr, const uint8_t *buf, uint32_t len);
    esp_err_t call(uint32_t addr, const uint32_t args[4], uint32_t stack_top, uint32_t timeout_ms, bool (*cancelled)(), uint32_t *r0);

    void (*on_progress)(uint32_t done, uint32_t total) = nullptr;
    static void trigger_nrst();
    static void reset_gpio();
};
