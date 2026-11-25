#include <cctype>
#include <cstdlib>
#include <cstring>
#include <strings.h>

#include <esp_err.h>
#include <esp_timer.h>

#include "host_priv.hpp"

#if CONFIG_SI_JS_ENABLE

namespace qjs_binder::priv
{
    JSValue target_error(JSContext *ctx, esp_err_t err, const char *code, const char *what)
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

    const char *code_of(esp_err_t err)
    {
        switch (err) {
        case ESP_ERR_TIMEOUT:
            return "TIMEOUT";
        case ESP_ERR_NOT_FINISHED:
            return "STOPPED"; // the run was cancelled
        case ESP_ERR_NOT_SUPPORTED:
            return "UNSUPPORTED";
        default:
            return "FAULT";
        }
    }

    const char *op_name(uint8_t op)
    {
        static const char *names[] = {"read32", "write32", "rmw32",  "halt", "readMem",    "writeMem",    "waitHalt",   "reset",
                                      "reinit", "poll32",  "delay",  "call", "flash.read", "flash.write", "flash.erase",
                                      "resume", "readReg", "writeReg", "watch", "done", "agent.incident", "incident data"};
        return op < sizeof(names) / sizeof(names[0]) ? names[op] : "?";
    }

    static bool is_xfer(uint8_t op)
    {
        return op == OP_READ_MEM || op == OP_WRITE_MEM || op == OP_FLASH_READ || op == OP_FLASH_WRITE;
    }

    // Promise calls that hold the owner up: a sync call would wait behind them.
    // A session's owner steps delay, poll32 and waitHalt between other work
    // (session.cpp, add_pending), so there they don't; a watch handler reading
    // memory while the module awaits target.delay() is the usual case.
    static int async_pending()
    {
        int n = 0;
        for (auto &s : g.slots) {
            bool stepped = g.session && (s.op == OP_DELAY || s.op == OP_POLL32 || s.op == OP_WAIT_HALT);
            n += s.used && !stepped;
        }
        return n;
    }

    void xfer_seen(uint32_t seq)
    {
        if (g.xfer_busy && (int32_t)(seq - g.xfer_seq) >= 0) {
            g.xfer_busy = false;
        }
    }

    static bool xfer_free(JSContext *ctx, const char *what)
    {
        if (g.xfer_busy) {
            res x;
            while (xQueueReceive(g.res_q, &x, 0) == pdTRUE) {
                xfer_seen(x.seq);
            }
        }
        if (g.xfer_busy) {
            JS_Throw(ctx, target_error(ctx, ESP_ERR_INVALID_STATE, "BUSY", what));
            return false;
        }
        return true;
    }

    static const char *needs(uint8_t op)
    {
        switch (op) {
        case OP_WRITE32:
        case OP_RMW32:
        case OP_WRITE_MEM:
        case OP_WRITE_REG:
        case OP_CALL:
            return g.cfg.perms.write ? nullptr : "--allow-write";
        case OP_FLASH_WRITE:
        case OP_FLASH_ERASE:
            return g.cfg.perms.flash ? nullptr : "--allow-flash";
        case OP_RESET:
        case OP_REINIT:
            return g.cfg.perms.reset ? nullptr : "--allow-reset";
        default:
            return nullptr;
        }
    }

    JSValue check_owned(JSContext *ctx, uint8_t op)
    {
        const char *what = op_name(op);
        if (g.session) { // the owner serves calls for the whole session
            const char *flag = needs(op);
            if (!flag) {
                return JS_UNDEFINED;
            }
            JSValue e = target_error(ctx, ESP_ERR_NOT_ALLOWED, "NOT_ALLOWED", what);
            if (!JS_IsException(e)) {
                char msg[96];
                snprintf(msg, sizeof(msg), "%s: NOT_ALLOWED, the %s was started without %s", what, g.cfg.incidents ? "trap" : "session", flag);
                JS_SetPropertyStr(ctx, e, "message", JS_NewString(ctx, msg));
            }
            return JS_Throw(ctx, e);
        }
        if (!g.hook_active || g.loading) {
            return JS_Throw(ctx, target_error(ctx, ESP_ERR_INVALID_STATE, "NOT_OWNED", what));
        }
        return JS_UNDEFINED;
    }

    static int get_u32(JSContext *ctx, JSValueConst v, uint32_t *out)
    {
        return JS_ToUint32(ctx, out, v);
    }

    int opt_u32(JSContext *ctx, JSValueConst opts, const char *name, uint32_t dflt, uint32_t *out)
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

    int get_bytes(JSContext *ctx, JSValueConst v, const uint8_t **p, size_t *len)
    {
        size_t off = 0, n = 0, per = 0;
        JSValue ab = JS_GetTypedArrayBuffer(ctx, v, &off, &n, &per);
        if (!JS_IsException(ab)) {
            size_t ab_len;
            uint8_t *base = JS_GetArrayBuffer(ctx, &ab_len, ab);
            JS_FreeValue(ctx, ab);
            if (!base || off > ab_len || n > ab_len - off) {
                JS_ThrowTypeError(ctx, "detached or out-of-range buffer");
                return -1;
            }
            *p = base + off;
            *len = n;
            return 0;
        }
        JS_FreeValue(ctx, JS_GetException(ctx)); // maybe an ArrayBuffer
        uint8_t *base = JS_GetArrayBuffer(ctx, len, v);
        if (!base) {
            return -1;
        }
        *p = base;
        return 0;
    }

    static JSValue sync_call(JSContext *ctx, req &r, JSValueConst opts, uint32_t *val)
    {
        const char *what = op_name(r.op);
        if (JS_IsException(check_owned(ctx, r.op))) {
            return JS_EXCEPTION;
        }
        uint32_t timeout_ms, retries;
        if (opt_u32(ctx, opts, "timeoutMs", CONFIG_SI_JS_CALL_TIMEOUT_MS, &timeout_ms) ||
            opt_u32(ctx, opts, "retries", 0, &retries)) {
            return JS_EXCEPTION;
        }
        if (async_pending()) {
            return JS_Throw(ctx, target_error(ctx, ESP_ERR_INVALID_STATE, "BUSY", what));
        }
        if (is_xfer(r.op) && !xfer_free(ctx, what)) {
            return JS_EXCEPTION;
        }

        r.slot = SYNC;
        r.retries = retries > 16 ? 16 : retries;
        r.seq = ++g.seq;
        if (xQueueSend(g.req_q, &r, 0) != pdTRUE) {
            return JS_Throw(ctx, target_error(ctx, ESP_ERR_INVALID_STATE, "BUSY", what));
        }
        if (is_xfer(r.op)) {
            g.xfer_busy = true;
            g.xfer_seq = r.seq;
        }

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
            xfer_seen(x.seq);
        }
        qjs_esp_loop_set_limits(g.loop, &g.lim);

        if (!got) {
            return JS_Throw(ctx, target_error(ctx, ESP_ERR_TIMEOUT, "TIMEOUT", what)); // buffer stays claimed
        }
        if (x.ret != ESP_OK) {
            xfer_seen(x.seq);
            bool reg = r.op == OP_READ_REG || r.op == OP_WRITE_REG;
            const char *code = reg && x.ret == ESP_ERR_INVALID_STATE ? "NOT_HALTED" : code_of(x.ret);
            return JS_Throw(ctx, target_error(ctx, x.ret, code, what));
        }
        *val = x.val;
        if (r.op != OP_READ_MEM) {
            xfer_seen(x.seq); // reads release after the copy
        }
        return JS_UNDEFINED;
    }

    JSValue async_call(JSContext *ctx, req &r)
    {
        const char *what = op_name(r.op);
        if (JS_IsException(check_owned(ctx, r.op))) {
            return JS_EXCEPTION;
        }
        int i = 0;
        while (i < MAX_ASYNC && g.slots[i].used) {
            i++;
        }
        if (i == MAX_ASYNC) {
            return JS_Throw(ctx, target_error(ctx, ESP_ERR_INVALID_STATE, "BUSY", what));
        }
        if (is_xfer(r.op) && !xfer_free(ctx, what)) {
            return JS_EXCEPTION;
        }

        JSValue funcs[2];
        JSValue p = JS_NewPromiseCapability(ctx, funcs);
        if (JS_IsException(p)) {
            return p;
        }
        r.slot = i;
        r.seq = ++g.seq;
        g.slots[i] = {true, r.op, r.seq, funcs[0], funcs[1], ESP_OK, 0}; // before the owner can answer
        if (xQueueSend(g.req_q, &r, 0) != pdTRUE) {
            g.slots[i].used = false;
            JS_FreeValue(ctx, funcs[0]);
            JS_FreeValue(ctx, funcs[1]);
            JS_FreeValue(ctx, p);
            return JS_Throw(ctx, target_error(ctx, ESP_ERR_INVALID_STATE, "BUSY", what));
        }
        if (is_xfer(r.op)) {
            g.xfer_busy = true;
            g.xfer_seq = r.seq;
        }
        return p;
    }

    void async_done(JSContext *ctx, void *arg)
    {
        auto &s = *static_cast<async_slot *>(arg);
        if (!s.used) {
            return; // the hook ended without waiting for it
        }
        s.used = false;
        JSValue v;
        if (s.ret != ESP_OK) {
            v = target_error(ctx, s.ret, code_of(s.ret), op_name(s.op));
        } else if (s.op == OP_FLASH_READ) {
            v = JS_NewUint8ArrayCopy(ctx, g.xfer, s.val); // val: bytes read
        } else {
            v = JS_NewUint32(ctx, s.val);
        }
        xfer_seen(s.seq); // buffer free from here
        bool ok = s.ret == ESP_OK && !JS_IsException(v);
        JSValue r = JS_Call(ctx, ok ? s.resolve : s.reject, JS_UNDEFINED, 1, &v);
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
        g.xfer_busy = false;
    }

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

    static JSValue js_resume(JSContext *ctx, JSValueConst, int argc, JSValueConst *argv)
    {
        req r = {};
        r.op = OP_RESUME;
        uint32_t val = 0;
        return sync_call(ctx, r, argc > 0 ? argv[0] : JS_UNDEFINED, &val);
    }

    static int get_reg(JSContext *ctx, JSValueConst v, uint32_t *out)
    {
        if (!JS_IsString(v)) {
            if (get_u32(ctx, v, out)) {
                return -1;
            }
            if (*out > 0x7f) {
                JS_ThrowRangeError(ctx, "register %lu: REGSEL is 0-127", (unsigned long)*out);
                return -1;
            }
            return 0;
        }
        static const struct {
            const char *name;
            uint8_t id;
        } names[] = {{"sp", 13}, {"lr", 14}, {"pc", 15}, {"xpsr", 16}, {"msp", 17}, {"psp", 18}, {"cfbp", 20}, {"fpscr", 33}};
        const char *s = JS_ToCString(ctx, v);
        if (!s) {
            return -1;
        }
        int id = -1;
        char c = (char)tolower((unsigned char)s[0]);
        if ((c == 'r' || c == 's') && isdigit((unsigned char)s[1])) {
            char *end;
            long n = strtol(s + 1, &end, 10);
            if (*end == '\0' && n <= (c == 'r' ? 15 : 31)) {
                id = c == 'r' ? (int)n : 0x40 + (int)n;
            }
        }
        for (size_t i = 0; id < 0 && i < sizeof(names) / sizeof(names[0]); i++) {
            if (strcasecmp(s, names[i].name) == 0) {
                id = names[i].id;
            }
        }
        if (id < 0) {
            JS_ThrowRangeError(ctx, "unknown register '%s'", s);
        }
        JS_FreeCString(ctx, s);
        *out = id;
        return id < 0 ? -1 : 0;
    }

    static JSValue js_read_reg(JSContext *ctx, JSValueConst, int argc, JSValueConst *argv)
    {
        req r = {};
        r.op = OP_READ_REG;
        uint32_t val = 0;
        if (get_reg(ctx, argv[0], &r.addr)) {
            return JS_EXCEPTION;
        }
        JSValue ret = sync_call(ctx, r, argc > 1 ? argv[1] : JS_UNDEFINED, &val);
        return JS_IsException(ret) ? ret : JS_NewUint32(ctx, val);
    }

    static JSValue js_write_reg(JSContext *ctx, JSValueConst, int argc, JSValueConst *argv)
    {
        req r = {};
        r.op = OP_WRITE_REG;
        uint32_t val = 0;
        if (get_reg(ctx, argv[0], &r.addr) || get_u32(ctx, argv[1], &r.a)) {
            return JS_EXCEPTION;
        }
        return sync_call(ctx, r, argc > 2 ? argv[2] : JS_UNDEFINED, &val);
    }

    static JSValue js_read_mem(JSContext *ctx, JSValueConst, int argc, JSValueConst *argv)
    {
        req r = {};
        r.op = OP_READ_MEM;
        uint32_t val = 0;
        if (get_u32(ctx, argv[0], &r.addr) || get_u32(ctx, argv[1], &r.a)) {
            return JS_EXCEPTION;
        }
        if (r.a > XFER_MAX) {
            return JS_ThrowRangeError(ctx, "readMem: at most %u bytes per call", (unsigned)XFER_MAX);
        }
        JSValue ret = sync_call(ctx, r, argc > 2 ? argv[2] : JS_UNDEFINED, &val);
        if (JS_IsException(ret)) {
            return ret;
        }
        JSValue out = JS_NewUint8ArrayCopy(ctx, g.xfer, r.a);
        xfer_seen(r.seq);
        return out;
    }

    static bool fill_xfer(JSContext *ctx, req &r, JSValueConst v)
    {
        const uint8_t *p;
        size_t len;
        if (get_bytes(ctx, v, &p, &len)) {
            return false;
        }
        if (len > XFER_MAX) {
            JS_ThrowRangeError(ctx, "at most %u bytes per call", (unsigned)XFER_MAX);
            return false;
        }
        if (JS_IsException(check_owned(ctx, r.op)) || !xfer_free(ctx, op_name(r.op))) {
            return false;
        }
        memcpy(g.xfer, p, len);
        r.a = len;
        return true;
    }

    static JSValue js_write_mem(JSContext *ctx, JSValueConst, int argc, JSValueConst *argv)
    {
        req r = {};
        r.op = OP_WRITE_MEM;
        uint32_t val = 0;
        if (get_u32(ctx, argv[0], &r.addr) || !fill_xfer(ctx, r, argv[1])) {
            return JS_EXCEPTION;
        }
        return sync_call(ctx, r, argc > 2 ? argv[2] : JS_UNDEFINED, &val);
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

    static JSValue js_call(JSContext *ctx, JSValueConst, int argc, JSValueConst *argv)
    {
        req r = {};
        r.op = OP_CALL;
        JSValueConst opts = argc > 2 ? argv[2] : JS_UNDEFINED;
        if (get_u32(ctx, argv[0], &r.addr) || opt_u32(ctx, opts, "timeoutMs", 1000, &r.timeout_ms) ||
            opt_u32(ctx, opts, "stackTop", 0, &r.b)) {
            return JS_EXCEPTION;
        }
        if (r.b == 0 || (r.b & 7)) {
            return JS_ThrowTypeError(ctx, "call: opts.stackTop must be an 8-byte aligned RAM address");
        }
        if (argc > 1 && !JS_IsUndefined(argv[1])) {
            int64_t n = 0;
            if (JS_GetLength(ctx, argv[1], &n)) {
                return JS_EXCEPTION;
            }
            if (n > 4) {
                return JS_ThrowRangeError(ctx, "call: at most 4 arguments");
            }
            for (int i = 0; i < n; i++) {
                JSValue v = JS_GetPropertyUint32(ctx, argv[1], i);
                int bad = get_u32(ctx, v, &r.args[i]);
                JS_FreeValue(ctx, v);
                if (bad) {
                    return JS_EXCEPTION;
                }
            }
        }
        return async_call(ctx, r);
    }

    static JSValue js_simple_async(JSContext *ctx, JSValueConst, int argc, JSValueConst *argv, int magic)
    {
        req r = {};
        r.op = magic;
        if (magic == OP_WAIT_HALT && opt_u32(ctx, argc > 0 ? argv[0] : JS_UNDEFINED, "timeoutMs", 1000, &r.timeout_ms)) {
            return JS_EXCEPTION; // the timeout is for agent sessions; job runs use the backend's
        }
        return async_call(ctx, r);
    }

    static JSValue js_watch(JSContext *ctx, JSValueConst, int argc, JSValueConst *argv)
    {
        if (!g.session) {
            return JS_ThrowTypeError(ctx, "watch: only in agent sessions");
        }
        return new_watch(ctx, argv[0], argc > 1 ? argv[1] : JS_UNDEFINED);
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

    static JSValue js_flash_read(JSContext *ctx, JSValueConst, int argc, JSValueConst *argv)
    {
        req r = {};
        r.op = OP_FLASH_READ;
        if (get_u32(ctx, argv[0], &r.addr) || get_u32(ctx, argv[1], &r.a)) {
            return JS_EXCEPTION;
        }
        if (r.a > XFER_MAX) {
            return JS_ThrowRangeError(ctx, "flash.read: at most %u bytes per call", (unsigned)XFER_MAX);
        }
        return async_call(ctx, r);
    }

    static JSValue js_flash_write(JSContext *ctx, JSValueConst, int argc, JSValueConst *argv)
    {
        req r = {};
        r.op = OP_FLASH_WRITE;
        if (get_u32(ctx, argv[0], &r.addr) || !fill_xfer(ctx, r, argv[1])) {
            return JS_EXCEPTION;
        }
        return async_call(ctx, r);
    }

    static JSValue js_flash_erase(JSContext *ctx, JSValueConst, int argc, JSValueConst *argv)
    {
        req r = {};
        r.op = OP_FLASH_ERASE;
        if (get_u32(ctx, argv[0], &r.addr) || get_u32(ctx, argv[1], &r.a)) {
            return JS_EXCEPTION;
        }
        return async_call(ctx, r);
    }

    static const JSCFunctionListEntry funcs[] = {
        JS_CFUNC_DEF("read32", 2, js_read32),
        JS_CFUNC_DEF("write32", 3, js_write32),
        JS_CFUNC_DEF("rmw32", 4, js_rmw32),
        JS_CFUNC_DEF("halt", 1, js_halt),
        JS_CFUNC_DEF("resume", 1, js_resume),
        JS_CFUNC_DEF("readReg", 2, js_read_reg),
        JS_CFUNC_DEF("writeReg", 3, js_write_reg),
        JS_CFUNC_DEF("readMem", 3, js_read_mem),
        JS_CFUNC_DEF("writeMem", 3, js_write_mem),
        JS_CFUNC_DEF("poll32", 4, js_poll32),
        JS_CFUNC_DEF("call", 3, js_call),
        JS_CFUNC_MAGIC_DEF("waitHalt", 1, js_simple_async, OP_WAIT_HALT),
        JS_CFUNC_MAGIC_DEF("reset", 0, js_simple_async, OP_RESET),
        JS_CFUNC_MAGIC_DEF("reinit", 0, js_simple_async, OP_REINIT),
        JS_CFUNC_DEF("delay", 1, js_delay),
        JS_CFUNC_DEF("watch", 2, js_watch),
    };

    static const JSCFunctionListEntry flash_funcs[] = {
        JS_CFUNC_DEF("read", 2, js_flash_read),
        JS_CFUNC_DEF("write", 2, js_flash_write),
        JS_CFUNC_DEF("erase", 2, js_flash_erase),
    };

    template <const JSCFunctionListEntry *tab, size_t n> static int module_init(JSContext *ctx, JSModuleDef *m)
    {
        return JS_SetModuleExportList(ctx, m, tab, n);
    }

    template <const JSCFunctionListEntry *tab, size_t n> static JSModuleDef *new_module(JSContext *ctx, const char *name)
    {
        JSModuleDef *m = JS_NewCModule(ctx, name, module_init<tab, n>);
        if (m && JS_AddModuleExportList(ctx, m, tab, n)) {
            return nullptr;
        }
        return m;
    }

    static bool add_object(JSContext *ctx, JSValueConst global, const char *name, const JSCFunctionListEntry *tab, size_t n)
    {
        JSValue o = JS_NewObject(ctx);
        if (JS_IsException(o) || JS_SetPropertyFunctionList(ctx, o, tab, n) < 0) {
            JS_FreeValue(ctx, o);
            return false;
        }
        return JS_SetPropertyStr(ctx, global, name, o) >= 0;
    }

    bool add_target_globals(JSContext *ctx, JSValueConst global)
    {
        return add_object(ctx, global, "target", funcs, sizeof(funcs) / sizeof(funcs[0])) &&
               add_object(ctx, global, "flash", flash_funcs, sizeof(flash_funcs) / sizeof(flash_funcs[0]));
    }

    JSModuleDef *init_target_module(JSContext *ctx)
    {
        if (!new_module<flash_funcs, sizeof(flash_funcs) / sizeof(flash_funcs[0])>(ctx, "si:flash")) {
            return nullptr;
        }
        return new_module<funcs, sizeof(funcs) / sizeof(funcs[0])>(ctx, "si:target");
    }
} // namespace qjs_binder::priv

#endif
