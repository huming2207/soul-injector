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
    };

    enum class hook : uint8_t {
        pre_program = 0,    // export async function preProgram(ctx)
        post_program = 1,   // export async function postProgram(ctx)
    };

    struct run_info {
        const char *job_name;
        const char *variant;
        const char *family;
        bool network;       // the job lets the script use esp:ws (CONFIG_SI_JS_NETWORK)
    };

    class js_host
    {
    public:
        static js_host *instance();

        esp_err_t init();

        esp_err_t begin_run(const char *path, const uint8_t *pinned_sha256, const run_info &info);

        esp_err_t begin_run(const uint8_t *blob, size_t len, const uint8_t *pinned_sha256, const run_info &info);

        bool has_hook(hook h) const;

        esp_err_t run_hook(hook h, js_target &target, uint32_t timeout_ms, cancel_fn cancelled);

        void end_run();

        bool last_script(uint8_t sha256[32], bool *pinned) const;

        void forget_last_script();

        const char *last_results() const;

        void progress(const char *stage, int pct);

        const char *last_error() const;

        js_host(js_host const &) = delete;
        void operator=(js_host const &) = delete;

    private:
        js_host() = default;
    };
} // namespace qjs_binder
