#pragma once

#include <cstddef>
#include <cstdint>

#include <esp_err.h>

/*
 * Job scripts: one JS task, a fresh QuickJS runtime per programming run in a
 * private PSRAM arena, and hooks the programming task calls around the C state
 * machine.
 *
 * The programming task keeps owning the target. While a hook runs it serves
 * the script's target calls (si:target) from a queue, so SWD never changes
 * task. Every wait on either side is bounded: a stuck script fails its hook,
 * and a JS task that doesn't come back after that disables scripting until
 * reboot while plain programming keeps working.
 */
namespace qjs_binder
{
    /*
     * The target as the script sees it. Implemented by the firmware over its
     * target backend; called only from the task that runs the hooks.
     */
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
    };

    enum class hook : uint8_t {
        pre_program = 0,    // export async function preProgram(ctx)
        post_program = 1,   // export async function postProgram(ctx)
    };

    /* Handed to every hook as ctx.job */
    struct run_info {
        const char *job_name;
        const char *variant;
        const char *family;
    };

    /* Polled while a hook runs; true stops the script */
    using cancel_fn = bool (*)();

    class js_host
    {
    public:
        static js_host *instance();

        /* Boot: arena, JS task, and the crash check. ESP_ERR_NOT_SUPPORTED if
         * CONFIG_SI_JS_ENABLE is off. Scripts are refused if this failed. */
        esp_err_t init();

        /*
         * Start a run: read the bytecode container at @p path, check it
         * against @p pinned_sha256 (32 bytes, or nullptr for no pin), then
         * load it in a fresh runtime and run its top level. Programming task.
         * Call end_run() afterwards whatever this returns.
         */
        esp_err_t begin_run(const char *path, const uint8_t *pinned_sha256, const run_info &info);

        /* Same, with the container in memory; it only has to stay valid until this returns */
        esp_err_t begin_run(const uint8_t *blob, size_t len, const uint8_t *pinned_sha256, const run_info &info);

        /* The loaded script exports this hook */
        bool has_hook(hook h) const;

        /*
         * Call a hook and serve its target calls until it settles, up to
         * @p timeout_ms (0: Kconfig default). ESP_FAIL: the script threw or
         * rejected; ESP_ERR_TIMEOUT: it ran out of time; ESP_ERR_INVALID_STATE:
         * the script isn't running (any more).
         */
        esp_err_t run_hook(hook h, js_target &target, uint32_t timeout_ms, cancel_fn cancelled);

        /* Tear the runtime down. No-op without a run. */
        void end_run();

        /* SHA-256 of the script the last begin_run() read. false if none. */
        bool last_script(uint8_t sha256[32], bool *pinned) const;

        /* Call when a programming run starts, so last_script() only answers for this one */
        void forget_last_script();

        /* One line about the last script failure, for the display */
        const char *last_error() const;

        js_host(js_host const &) = delete;
        void operator=(js_host const &) = delete;

    private:
        js_host() = default;
    };
} // namespace qjs_binder
