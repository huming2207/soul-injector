#pragma once

#include <atomic>
#include <cstdint>

#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <freertos/semphr.h>

#include "qjs_binder.hpp"
#include "qjs_esp.h"
#include "quickjs.h"

namespace qjs_binder::priv
{
    enum op : uint8_t {
        OP_READ32,
        OP_WRITE32,
        OP_RMW32,
        OP_HALT,
        OP_READ_MEM,    // xfer
        OP_WRITE_MEM,   // xfer
        OP_WAIT_HALT,
        OP_RESET,
        OP_REINIT,
        OP_POLL32,
        OP_DELAY,
        OP_CALL,
        OP_FLASH_READ,  // xfer
        OP_FLASH_WRITE, // xfer
        OP_FLASH_ERASE,
        OP_RESUME,
        OP_READ_REG,
        OP_WRITE_REG,
        OP_DONE, // load/hook settled
    };

    static constexpr uint8_t SYNC = 0xff;
    static constexpr int MAX_ASYNC = 4;
    static constexpr int HOOKS = 4;

    struct req {
        uint8_t op;
        uint8_t slot;    // SYNC, or the async slot to complete
        uint8_t retries; // sync ops
        uint32_t seq;
        uint32_t addr;
        uint32_t a;      // value, and-mask, poll mask, delay ms, length
        uint32_t b;      // or-bits, poll expect, call stack top
        uint32_t args[4];
        uint32_t timeout_ms;
        uint32_t interval_ms;
        esp_err_t ret;   // OP_DONE
    };

    struct res {
        uint32_t seq;
        esp_err_t ret;
        uint32_t val;
    };

    struct async_slot {
        bool used;
        uint8_t op;
        uint32_t seq;
        JSValue resolve; // JS task only
        JSValue reject;
        esp_err_t ret;   // set by owner before posting
        uint32_t val;
    };

    struct state {
        qjs_esp_arena_t *arena;
        QueueHandle_t cmd_q;
        QueueHandle_t req_q;
        QueueHandle_t res_q;
        SemaphoreHandle_t ended;     // JS task gave the runtime back
        SemaphoreHandle_t loop_lock; // owner stops it, JS task frees it
        bool ready;
        std::atomic<bool> dead;      // no scripts until reboot

        uint8_t *xfer;
        bool xfer_busy;              // JS task
        uint32_t xfer_seq;

        qjs_esp_loop_t *loop;        // under loop_lock when written
        JSModuleDef *mod;
        qjs_esp_limits_t lim;
        bool loading;                // the top level is running
        bool hook_active;            // a hook is in flight
        uint32_t seq;
        async_slot slots[MAX_ASYNC];

        const uint8_t *script;
        size_t script_len;
        hook cur_hook;
        uint32_t cur_timeout_ms;
        char job_name[33];
        char variant[33];
        char family[16];

        bool network;
        JSValue results;             // log.record/check, JS task
        JSValue on_progress;         // job.onProgress callback, JS task

        std::atomic<const char *> prog_stage;
        std::atomic<int> prog_pct;
        std::atomic<bool> prog_posted;
        const char *prog_last_stage; // owner
        int64_t prog_last_us;        // owner

        bool hooks[HOOKS];
        std::atomic<bool> alive;     // the loop takes hooks
        char fail_stage[16];         // onFail arguments
        char fail_reason[96];
        char err[96];
        char results_json[RESULTS_MAX + 1];

        bool running;
        int64_t hook_end_us;         // async ops stop here
        uint8_t sha256[32];
        bool have_sha;
        bool pinned;
    };

    extern state g;

    JSModuleDef *init_target_module(JSContext *ctx);
    void async_done(JSContext *ctx, void *arg);
    void async_reset(JSContext *ctx);
    void xfer_seen(uint32_t seq);

    bool init_misc_modules(JSContext *ctx);
    void progress_cb(JSContext *ctx, void *arg);
    JSValue target_error(JSContext *ctx, esp_err_t err, const char *code, const char *what);
    const char *code_of(esp_err_t err);
    JSValue async_call(JSContext *ctx, req &r);
    JSValue check_owned(JSContext *ctx, const char *what);
    int opt_u32(JSContext *ctx, JSValueConst opts, const char *name, uint32_t dflt, uint32_t *out);
    int get_bytes(JSContext *ctx, JSValueConst v, const uint8_t **p, size_t *len);
} // namespace qjs_binder::priv
