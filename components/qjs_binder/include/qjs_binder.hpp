#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>

#include <esp_err.h>

namespace qjs_binder
{
    using cancel_fn = bool (*)();

    static constexpr size_t XFER_MAX = 4096;
    static constexpr size_t RESULTS_MAX = 359;

    struct core_state {
        bool halted;
        bool lockup;
        bool reset_seen; // the core was reset since the last poll (S_RESET_ST)
        uint32_t dfsr;   // why it halted (Cortex-M DFSR), when halted
    };

    class js_target
    {
    public:
        virtual ~js_target() = default;
        virtual esp_err_t read32(uint32_t addr, uint32_t *val) = 0;
        virtual esp_err_t write32(uint32_t addr, uint32_t val) = 0;
        virtual esp_err_t halt() = 0;
        virtual esp_err_t wait_halt() = 0;
        virtual esp_err_t reset() = 0;
        virtual esp_err_t reinit() = 0;

        virtual esp_err_t read_mem(uint32_t addr, uint8_t *buf, size_t len)
        {
            for (size_t i = 0; i < len; i += 4) {
                uint32_t w;
                esp_err_t ret = (addr | len) & 3 ? ESP_ERR_INVALID_ARG : read32(addr + i, &w);
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
                uint32_t w;
                memcpy(&w, buf + i, 4);
                esp_err_t ret = (addr | len) & 3 ? ESP_ERR_INVALID_ARG : write32(addr + i, w);
                if (ret != ESP_OK) {
                    return ret;
                }
            }
            return ESP_OK;
        }

        virtual esp_err_t flash_read(uint32_t addr, uint8_t *buf, size_t len) { return ESP_ERR_NOT_SUPPORTED; }
        virtual esp_err_t flash_write(uint32_t addr, const uint8_t *buf, size_t len) { return ESP_ERR_NOT_SUPPORTED; }
        virtual esp_err_t flash_erase(uint32_t addr, size_t len) { return ESP_ERR_NOT_SUPPORTED; }

        virtual esp_err_t call(uint32_t addr, const uint32_t args[4], uint32_t stack_top, uint32_t timeout_ms,
                               cancel_fn cancelled, uint32_t *r0)
        {
            return ESP_ERR_NOT_SUPPORTED;
        }

        virtual esp_err_t resume() { return ESP_ERR_NOT_SUPPORTED; }
        virtual esp_err_t read_reg(uint32_t id, uint32_t *val) { return ESP_ERR_NOT_SUPPORTED; } // id: DCRSR REGSEL
        virtual esp_err_t write_reg(uint32_t id, uint32_t val) { return ESP_ERR_NOT_SUPPORTED; }

        virtual esp_err_t poll_core(core_state *st) { return ESP_ERR_NOT_SUPPORTED; } // agent sessions from here on
        virtual esp_err_t set_vector_catch(uint32_t vc) { return ESP_ERR_NOT_SUPPORTED; } // DEMCR VC_* bits, others kept
        virtual esp_err_t clear_stop_cause() { return ESP_ERR_NOT_SUPPORTED; } // DFSR
    };

    struct permissions {
        bool write; // memory and core register writes, call()
        bool flash; // flash.erase(), flash.write()
        bool reset; // reset(), reinit(), 'reset' reflexes and verdicts
    };

    struct session_config {
        permissions perms;
        bool fault_regs;                     // the target has CFSR, HFSR... (ARMv7-M and up)
        void (*notify)(uint32_t newest_seq); // new events; any task, must not block
    };

    enum class script_outcome : uint8_t { ok, error, pending, stopped };

    struct script_result {
        script_outcome outcome;
        bool truncated;
        const char *value; // eval: the result as JSON
        const char *error;
        const char *stack;
    };

    static constexpr size_t EVENT_KIND_MAX = 11;
    static constexpr size_t EVENT_JSON_MAX = 767;
    static constexpr size_t RESULT_VALUE_MAX = 4095;
    static constexpr size_t RESULT_ERROR_MAX = 191;
    static constexpr size_t RESULT_STACK_MAX = 1023;

    struct event_out {
        uint32_t seq;
        uint64_t uptime_us;
        uint32_t capture_id;
        char kind[EVENT_KIND_MAX + 1];
        char json[EVENT_JSON_MAX + 1];
    };

    enum class hook : uint8_t {
        pre_program = 0,    // export async function preProgram(ctx)
        post_program = 1,   // export async function postProgram(ctx)
        test = 2,           // export async function test(ctx), replaces the job's self tests
        on_fail = 3,        // export async function onFail(ctx, stage, err), run with run_on_fail()
    };

    struct run_info {
        const char *job_name;
        const char *variant;
        const char *family;
        bool network;       // the job lets the script use esp:ws (CONFIG_SI_JS_NETWORK)
    };

    struct engine_info {
        const char *version; // QuickJS-NG version, "" when scripts can't run
        uint32_t arena_kb;
        bool network;        // CONFIG_SI_JS_NETWORK
    };

    class js_host
    {
    public:
        static js_host *instance();

        engine_info engine() const;

        esp_err_t init();

        esp_err_t begin_run(const char *path, const uint8_t *pinned_sha256, const run_info &info);

        esp_err_t begin_run(const uint8_t *blob, size_t len, const uint8_t *pinned_sha256, const run_info &info);

        bool has_hook(hook h) const;

        esp_err_t run_hook(hook h, js_target &target, uint32_t timeout_ms, cancel_fn cancelled);

        esp_err_t run_on_fail(const char *stage, const char *reason, js_target &target, uint32_t timeout_ms,
                              cancel_fn cancelled); // keeps last_error(); reason nullptr: the script's error

        void end_run();

        bool last_script(uint8_t sha256[32], bool *pinned) const;

        void forget_last_script();

        const char *last_results() const;

        void progress(const char *stage, int pct);

        const char *last_error() const;

        esp_err_t begin_session(const session_config &cfg); // the calling task owns the target until end_session()

        esp_err_t session_run(const uint8_t *blob, size_t len, uint32_t timeout_ms, js_target &target); // replaces the runtime

        esp_err_t session_eval(const uint8_t *blob, size_t len, uint32_t timeout_ms, js_target &target);

        void session_stop(js_target &target);

        void session_service(js_target &target, uint32_t wait_ms); // serve calls, watches; call it often

        void end_session(js_target &target);

        bool in_session() const;

        script_result last_script_result() const;

        size_t read_events(uint32_t after_seq, event_out *out, size_t max, uint32_t *newest_seq, uint32_t *dropped);

        esp_err_t read_capture(uint32_t id, uint32_t offset, uint8_t *buf, size_t cap, size_t *len, uint32_t *total);

        js_host(js_host const &) = delete;
        void operator=(js_host const &) = delete;

    private:
        js_host() = default;
    };
} // namespace qjs_binder
