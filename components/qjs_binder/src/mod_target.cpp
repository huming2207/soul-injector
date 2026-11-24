/*
 * si:target - the target, as far as a job script may touch it.
 *
 *   read32(addr, opts?)            -> number      sync
 *   write32(addr, value, opts?)                   sync
 *   rmw32(addr, andMask, orBits, opts?) -> value  sync, value = (old & andMask) | orBits
 *   halt(opts?)                                   sync
 *   poll32(addr, mask, expect, { timeoutMs = 1000, intervalMs = 1 }) -> Promise<value>
 *   waitHalt() / reset() / reinit() -> Promise
 *   delay(ms) -> Promise
 *
 * Sync opts: { timeoutMs = CONFIG_SI_JS_CALL_TIMEOUT_MS, retries = 0 }. Calls
 * only work inside a hook. Failures throw (or reject with) a TargetError:
 * .code is NOT_OWNED, BUSY, TIMEOUT, STOPPED or FAULT, .err the esp_err_t.
 */

#include <cstring>

#include <esp_err.h>
#include <esp_timer.h>

#include "host_priv.hpp"

#if CONFIG_SI_JS_ENABLE

namespace qjs_binder::priv
{
    static JSValue target_error(JSContext *ctx, esp_err_t err, const char *code, const char *what)
    {
        JSValue e = JS_NewError(ctx);
        if (JS_IsException(e)) {
            return e;
        }
        char msg[96];
        snprintf(msg, sizeof(msg), "%s: %s (%s)", what, code, esp_err_to_name(err));
        JS_SetPropertyStr(ctx, e, "name", JS_NewString(ctx, "TargetError"));
        JS_SetPropertyStr(ctx, e, "message", JS_NewString(ctx, msg));
        JS_SetPropertyStr(ctx, e, "code", JS_NewString(ctx, code));
        JS_SetPropertyStr(ctx, e, "err", JS_NewInt32(ctx, err));
        return e;
    }

    static const char *code_of(esp_err_t err)
    {
        switch (err) {
        case ESP_ERR_TIMEOUT:
            return "TIMEOUT";
        case ESP_ERR_INVALID_STATE:
            return "STOPPED"; // cancelled, or no target to talk to
        default:
            return "FAULT";
        }
    }

    static const char *op_name(uint8_t op)
    {
        static const char *names[] = {"read32", "write32", "rmw32", "halt", "waitHalt", "reset", "reinit", "poll32", "delay"};
        return op < sizeof(names) / sizeof(names[0]) ? names[op] : "?";
    }

    static int async_pending()
    {
        int n = 0;
        for (auto &s : g.slots) {
            n += s.used;
        }
        return n;
    }

    /* Target calls only while a hook runs: that's when the owner serves them */
    static JSValue check_owned(JSContext *ctx, const char *what)
    {
        if (!g.hook_active || g.loading) {
            return JS_Throw(ctx, target_error(ctx, ESP_ERR_INVALID_STATE, "NOT_OWNED", what));
        }
        return JS_UNDEFINED;
    }

    static int get_u32(JSContext *ctx, JSValueConst v, uint32_t *out)
    {
        return JS_ToUint32(ctx, out, v);
    }

    /* opts.name as uint32, or dflt if absent */
    static int opt_u32(JSContext *ctx, JSValueConst opts, const char *name, uint32_t dflt, uint32_t *out)
    {
        *out = dflt;
        if (!JS_IsObject(opts)) {
            return 0;
        }
        JSValue v = JS_GetPropertyStr(ctx, opts, name);
        int ret = JS_IsUndefined(v) ? 0 : get_u32(ctx, v, out);
        JS_FreeValue(ctx, v);
        return ret;
    }

    static JSValue sync_call(JSContext *ctx, req &r, JSValueConst opts, uint32_t *val)
    {
        const char *what = op_name(r.op);
        if (JS_IsException(check_owned(ctx, what))) {
            return JS_EXCEPTION;
        }
        uint32_t timeout_ms, retries;
        if (opt_u32(ctx, opts, "timeoutMs", CONFIG_SI_JS_CALL_TIMEOUT_MS, &timeout_ms) ||
            opt_u32(ctx, opts, "retries", 0, &retries)) {
            return JS_EXCEPTION;
        }
        if (async_pending()) {
            // it would queue behind a poll or a delay and time out
            return JS_Throw(ctx, target_error(ctx, ESP_ERR_INVALID_STATE, "BUSY", what));
        }

        r.slot = SYNC;
        r.retries = retries > 16 ? 16 : retries;
        r.seq = ++g.seq;
        if (xQueueSend(g.req_q, &r, 0) != pdTRUE) {
            return JS_Throw(ctx, target_error(ctx, ESP_ERR_INVALID_STATE, "BUSY", what));
        }

        // Late answers to an earlier call that timed out carry an older seq
        int64_t end = esp_timer_get_time() + timeout_ms * 1000LL;
        res x = {};
        bool got = false;
        for (;;) {
            int64_t left_ms = (end - esp_timer_get_time()) / 1000;
            if (left_ms <= 0 || xQueueReceive(g.res_q, &x, pdMS_TO_TICKS(left_ms) ? pdMS_TO_TICKS(left_ms) : 1) != pdTRUE) {
                break;
            }
            if (x.seq == r.seq) {
                got = true;
                break;
            }
        }
        // Waiting on the target isn't script CPU time: restart the slice
        qjs_esp_loop_set_limits(g.loop, &g.lim);

        if (!got) {
            return JS_Throw(ctx, target_error(ctx, ESP_ERR_TIMEOUT, "TIMEOUT", what));
        }
        if (x.ret != ESP_OK) {
            return JS_Throw(ctx, target_error(ctx, x.ret, code_of(x.ret), what));
        }
        *val = x.val;
        return JS_UNDEFINED;
    }

    static JSValue async_call(JSContext *ctx, req &r)
    {
        const char *what = op_name(r.op);
        if (JS_IsException(check_owned(ctx, what))) {
            return JS_EXCEPTION;
        }
        int i = 0;
        while (i < MAX_ASYNC && g.slots[i].used) {
            i++;
        }
        if (i == MAX_ASYNC) {
            return JS_Throw(ctx, target_error(ctx, ESP_ERR_INVALID_STATE, "BUSY", what));
        }

        JSValue funcs[2];
        JSValue p = JS_NewPromiseCapability(ctx, funcs);
        if (JS_IsException(p)) {
            return p;
        }
        r.slot = i;
        r.seq = ++g.seq;
        if (xQueueSend(g.req_q, &r, 0) != pdTRUE) {
            JS_FreeValue(ctx, funcs[0]);
            JS_FreeValue(ctx, funcs[1]);
            JS_FreeValue(ctx, p);
            return JS_Throw(ctx, target_error(ctx, ESP_ERR_INVALID_STATE, "BUSY", what));
        }
        g.slots[i] = {true, funcs[0], funcs[1], ESP_OK, 0};
        return p;
    }

    void async_done(JSContext *ctx, void *arg)
    {
        auto &s = *static_cast<async_slot *>(arg);
        if (!s.used) {
            return; // the hook ended without waiting for it
        }
        s.used = false;
        JSValue v = s.ret == ESP_OK ? JS_NewUint32(ctx, s.val) : target_error(ctx, s.ret, code_of(s.ret), "async call");
        JSValue r = JS_Call(ctx, s.ret == ESP_OK ? s.resolve : s.reject, JS_UNDEFINED, 1, &v);
        JS_FreeValue(ctx, r);
        JS_FreeValue(ctx, v);
        JS_FreeValue(ctx, s.resolve);
        JS_FreeValue(ctx, s.reject);
    }

    void async_reset(JSContext *ctx)
    {
        for (auto &s : g.slots) {
            if (s.used) {
                JS_FreeValue(ctx, s.resolve);
                JS_FreeValue(ctx, s.reject);
                s.used = false;
            }
        }
    }

    // ---- functions ----

    static JSValue js_read32(JSContext *ctx, JSValueConst, int argc, JSValueConst *argv)
    {
        req r = {};
        r.op = OP_READ32;
        uint32_t val = 0;
        if (get_u32(ctx, argv[0], &r.addr)) {
            return JS_EXCEPTION;
        }
        JSValue ret = sync_call(ctx, r, argc > 1 ? argv[1] : JS_UNDEFINED, &val);
        return JS_IsException(ret) ? ret : JS_NewUint32(ctx, val);
    }

    static JSValue js_write32(JSContext *ctx, JSValueConst, int argc, JSValueConst *argv)
    {
        req r = {};
        r.op = OP_WRITE32;
        uint32_t val = 0;
        if (get_u32(ctx, argv[0], &r.addr) || get_u32(ctx, argv[1], &r.a)) {
            return JS_EXCEPTION;
        }
        return sync_call(ctx, r, argc > 2 ? argv[2] : JS_UNDEFINED, &val);
    }

    static JSValue js_rmw32(JSContext *ctx, JSValueConst, int argc, JSValueConst *argv)
    {
        req r = {};
        r.op = OP_RMW32;
        uint32_t val = 0;
        if (get_u32(ctx, argv[0], &r.addr) || get_u32(ctx, argv[1], &r.a) || get_u32(ctx, argv[2], &r.b)) {
            return JS_EXCEPTION;
        }
        JSValue ret = sync_call(ctx, r, argc > 3 ? argv[3] : JS_UNDEFINED, &val);
        return JS_IsException(ret) ? ret : JS_NewUint32(ctx, val);
    }

    static JSValue js_halt(JSContext *ctx, JSValueConst, int argc, JSValueConst *argv)
    {
        req r = {};
        r.op = OP_HALT;
        uint32_t val = 0;
        return sync_call(ctx, r, argc > 0 ? argv[0] : JS_UNDEFINED, &val);
    }

    static JSValue js_poll32(JSContext *ctx, JSValueConst, int argc, JSValueConst *argv)
    {
        req r = {};
        r.op = OP_POLL32;
        JSValueConst opts = argc > 3 ? argv[3] : JS_UNDEFINED;
        if (get_u32(ctx, argv[0], &r.addr) || get_u32(ctx, argv[1], &r.a) || get_u32(ctx, argv[2], &r.b) ||
            opt_u32(ctx, opts, "timeoutMs", 1000, &r.timeout_ms) || opt_u32(ctx, opts, "intervalMs", 1, &r.interval_ms)) {
            return JS_EXCEPTION;
        }
        return async_call(ctx, r);
    }

    static JSValue js_simple_async(JSContext *ctx, JSValueConst, int argc, JSValueConst *argv, int magic)
    {
        req r = {};
        r.op = magic;
        return async_call(ctx, r);
    }

    static JSValue js_delay(JSContext *ctx, JSValueConst, int argc, JSValueConst *argv)
    {
        req r = {};
        r.op = OP_DELAY;
        if (get_u32(ctx, argv[0], &r.a)) {
            return JS_EXCEPTION;
        }
        return async_call(ctx, r);
    }

    static const JSCFunctionListEntry funcs[] = {
        JS_CFUNC_DEF("read32", 2, js_read32),
        JS_CFUNC_DEF("write32", 3, js_write32),
        JS_CFUNC_DEF("rmw32", 4, js_rmw32),
        JS_CFUNC_DEF("halt", 1, js_halt),
        JS_CFUNC_DEF("poll32", 4, js_poll32),
        JS_CFUNC_MAGIC_DEF("waitHalt", 0, js_simple_async, OP_WAIT_HALT),
        JS_CFUNC_MAGIC_DEF("reset", 0, js_simple_async, OP_RESET),
        JS_CFUNC_MAGIC_DEF("reinit", 0, js_simple_async, OP_REINIT),
        JS_CFUNC_DEF("delay", 1, js_delay),
    };

    static int module_init(JSContext *ctx, JSModuleDef *m)
    {
        return JS_SetModuleExportList(ctx, m, funcs, sizeof(funcs) / sizeof(funcs[0]));
    }

    JSModuleDef *init_target_module(JSContext *ctx)
    {
        JSModuleDef *m = JS_NewCModule(ctx, "si:target", module_init);
        if (m && JS_AddModuleExportList(ctx, m, funcs, sizeof(funcs) / sizeof(funcs[0]))) {
            return nullptr;
        }
        return m;
    }
} // namespace qjs_binder::priv

#endif
