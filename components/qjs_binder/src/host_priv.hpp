#pragma once

/*
 * Shared between js_host.cpp (task plumbing) and mod_target.cpp (si:target).
 *
 * Two tasks: the JS task runs the script; the owner (the programming task)
 * owns the target and serves the script's calls while a hook runs.
 *   owner -> JS:   cmd_q (start a run), qjs_esp_loop_post (call a hook,
 *                  async results), qjs_esp_loop_stop (end, cancel, timeout)
 *   JS -> owner:   req_q (target calls, OP_DONE when a load/hook settles)
 *   owner -> JS:   res_q (sync results, depth 1, matched by seq)
 */

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
        OP_WAIT_HALT,
        OP_RESET,
        OP_REINIT,
        OP_POLL32,
        OP_DELAY,
        OP_DONE, // the load or the hook settled; ret says how
    };

    static constexpr uint8_t SYNC = 0xff;
    static constexpr int MAX_ASYNC = 4;

    struct req {
        uint8_t op;
        uint8_t slot;    // SYNC, or the async slot to complete
        uint8_t retries; // extra attempts after a failure (sync ops)
        uint32_t seq;
        uint32_t addr;
        uint32_t a;      // value, and-mask, poll mask, delay ms
        uint32_t b;      // or-bits, poll expect
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
        JSValue resolve; // JS task only
        JSValue reject;
        esp_err_t ret;   // written by the owner before it posts the slot
        uint32_t val;
    };

    struct state {
        // Boot
        qjs_esp_arena_t *arena;
        QueueHandle_t cmd_q;
        QueueHandle_t req_q;
        QueueHandle_t res_q;
        SemaphoreHandle_t ended;     // JS task gave the runtime back
        SemaphoreHandle_t loop_lock; // owner may stop the loop while the JS task frees it
        bool ready;
        std::atomic<bool> dead;      // JS task stopped answering: no scripts until reboot

        // JS task, one run
        qjs_esp_loop_t *loop;        // under loop_lock when written
        JSModuleDef *mod;
        qjs_esp_limits_t lim;
        bool loading;                // the top level is running
        bool hook_active;            // a hook is in flight
        uint32_t seq;
        async_slot slots[MAX_ASYNC];

        // Owner writes before handing over, JS task reads
        const uint8_t *script;
        size_t script_len;
        hook cur_hook;
        uint32_t cur_timeout_ms;
        char job_name[33];
        char variant[33];
        char family[16];

        // JS task writes before OP_DONE, owner reads after
        bool hooks[2];
        char err[96];

        // Owner
        bool running;
        int64_t hook_end_us;         // async ops on the owner don't outlive the hook
        uint8_t sha256[32];
        bool have_sha;
        bool pinned;
    };

    extern state g;

    /* si:target, registered on every new context */
    JSModuleDef *init_target_module(JSContext *ctx);
    /* Owner -> JS: complete an async call (qjs_esp_loop_post callback) */
    void async_done(JSContext *ctx, void *arg);
    /* JS task: drop async calls still pending when a hook ends */
    void async_reset(JSContext *ctx);
} // namespace qjs_binder::priv
