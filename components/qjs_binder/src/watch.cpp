#include <algorithm>
#include <cstdio>
#include <cstring>

#include <esp_heap_caps.h>
#include <esp_log.h>

#include "host_priv.hpp"

#if CONFIG_SI_JS_ENABLE

namespace qjs_binder::priv
{
    static const char *TAG = "js_watch";
    static constexpr int64_t POLL_US = 5000;
    static constexpr int64_t LOST_POLL_US = 500000;
    static constexpr uint8_t LOST_AFTER = 3;
    static constexpr uint16_t BURST_MAX = 10;     // fires per watch per second before it's throttled
    static constexpr uint32_t HANDLER_TIMEOUT_MS = 5000;
    static constexpr uint32_t HANDLER_TIMEOUT_MAX = 60000;
    static constexpr uint32_t STACK_DEFAULT = 256;
    static constexpr uint32_t RANGE_MAX = 4096;
    static constexpr uint32_t READ_CHUNK = 256;

    static constexpr uint32_t DFSR_BKPT = 1u << 1;
    static constexpr uint32_t DFSR_DWTTRAP = 1u << 2;
    static constexpr uint32_t DFSR_VCATCH = 1u << 3;
    static constexpr uint32_t DFSR_EXTERNAL = 1u << 4;
    static constexpr uint32_t DFSR_STOP_CAUSES = DFSR_BKPT | DFSR_DWTTRAP | DFSR_VCATCH | DFSR_EXTERNAL; // not HALTED: a halt request
    static constexpr uint32_t VC_CORERESET = 1u << 0;
    static constexpr uint32_t VC_FAULTS = 0x7F0; // MMERR NOCPERR CHKERR STATERR BUSERR INTERR HARDERR

    static constexpr uint8_t SEC_REGS = 1, SEC_FAULT = 2, SEC_MEM = 3, SEC_STOP = 4;
    static constexpr uint8_t MEM_STACK = 0, MEM_RANGE = 1;
    static const uint8_t REG_IDS[] = {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16, 17, 18, 20};
    static const uint32_t FAULT_ADDRS[] = {0xE000ED24, 0xE000ED28, 0xE000ED2C, 0xE000ED30, 0xE000ED34, 0xE000ED38, 0xE000ED3C};
    static const char *const FAULT_NAMES[] = {"shcsr", "cfsr", "hfsr", "dfsr", "mmfar", "bfar", "afsr"};
    static const char *const ON_NAMES[WATCHES] = {"fault", "halt", "lockup", "reset"};
    static const char *const REFLEX_NAMES[] = {"halt", "none", "reset"};

    static void put32(uint8_t *p, uint32_t v)
    {
        memcpy(p, &v, 4); // little endian target and wire
    }

    static uint32_t get32(const uint8_t *p)
    {
        uint32_t v;
        memcpy(&v, p, 4);
        return v;
    }

    static int index_of(const char *const *names, int n, const char *s)
    {
        for (int i = 0; i < n; i++) {
            if (strcmp(names[i], s) == 0) {
                return i;
            }
        }
        return -1;
    }

    static int opt_bool(JSContext *ctx, JSValueConst obj, const char *name, bool dflt, bool *out)
    {
        *out = dflt;
        if (!JS_IsObject(obj)) {
            return 0;
        }
        JSValue v = JS_GetPropertyStr(ctx, obj, name);
        if (JS_IsException(v)) {
            return -1;
        }
        if (!JS_IsUndefined(v)) {
            *out = JS_ToBool(ctx, v);
        }
        JS_FreeValue(ctx, v);
        return 0;
    }

    static int opt_str(JSContext *ctx, JSValueConst obj, const char *name, char *out, size_t cap, bool *given)
    {
        *given = false;
        out[0] = '\0';
        JSValue v = JS_GetPropertyStr(ctx, obj, name);
        if (JS_IsException(v)) {
            return -1;
        }
        if (JS_IsUndefined(v)) {
            return 0;
        }
        size_t len = 0;
        const char *s = JS_ToCStringLen(ctx, &len, v);
        JS_FreeValue(ctx, v);
        if (!s) {
            return -1;
        }
        int ret = 0;
        if (len >= cap) {
            JS_ThrowRangeError(ctx, "watch: %s is longer than %u bytes", name, (unsigned)(cap - 1));
            ret = -1;
        } else {
            memcpy(out, s, len + 1);
            *given = true;
        }
        JS_FreeCString(ctx, s);
        return ret;
    }

    static int parse_mem(JSContext *ctx, JSValueConst list, watch_def &d)
    {
        if (JS_IsUndefined(list)) {
            return 0;
        }
        int64_t n = 0;
        if (!JS_IsArray(list) || JS_GetLength(ctx, list, &n)) {
            JS_ThrowTypeError(ctx, "watch: capture.mem must be an array of [address, length]");
            return -1;
        }
        if (n > WATCH_MEM) {
            JS_ThrowRangeError(ctx, "watch: at most %d capture.mem ranges", WATCH_MEM);
            return -1;
        }
        for (int64_t i = 0; i < n; i++) {
            JSValue pair = JS_GetPropertyUint32(ctx, list, i);
            JSValue a = JS_GetPropertyUint32(ctx, pair, 0);
            JSValue l = JS_GetPropertyUint32(ctx, pair, 1);
            uint32_t addr = 0, len = 0;
            int bad = !JS_IsArray(pair) || JS_ToUint32(ctx, &addr, a) || JS_ToUint32(ctx, &len, l);
            JS_FreeValue(ctx, a);
            JS_FreeValue(ctx, l);
            JS_FreeValue(ctx, pair);
            if (bad) {
                if (!JS_HasException(ctx)) {
                    JS_ThrowTypeError(ctx, "watch: capture.mem must be an array of [address, length]");
                }
                return -1;
            }
            if ((addr | len) & 3 || len == 0 || len > RANGE_MAX) {
                JS_ThrowRangeError(ctx, "watch: capture.mem [0x%lx, %lu]: 4-byte aligned, 4-%u bytes", (unsigned long)addr,
                                   (unsigned long)len, (unsigned)RANGE_MAX);
                return -1;
            }
            d.mem[d.n_mem].addr = addr;
            d.mem[d.n_mem].len = len;
            d.n_mem++;
        }
        return 0;
    }

    static JSValue busy_error(JSContext *ctx, const char *msg)
    {
        JSValue e = target_error(ctx, ESP_ERR_INVALID_STATE, "BUSY", "watch");
        if (!JS_IsException(e)) {
            JS_SetPropertyStr(ctx, e, "message", JS_NewString(ctx, msg));
        }
        return JS_Throw(ctx, e);
    }

    static JSValue js_cancel(JSContext *ctx, JSValueConst, int, JSValueConst *, int, JSValueConst *data)
    {
        int32_t on = 0, id = 0;
        JS_ToInt32(ctx, &on, data[0]);
        JS_ToInt32(ctx, &id, data[1]);
        if (on < 0 || on >= WATCHES) {
            return JS_FALSE;
        }
        xSemaphoreTake(g.watch_lock, portMAX_DELAY);
        bool mine = g.watches[on].used && g.watches[on].id == id;
        if (mine) {
            g.watches[on] = {};
        }
        xSemaphoreGive(g.watch_lock);
        if (!mine) {
            return JS_FALSE;
        }
        g.watch_gen++;
        JS_FreeValue(ctx, g.watch_fn[on]);
        g.watch_fn[on] = JS_UNDEFINED;
        return JS_TRUE;
    }

    static JSValue describe(JSContext *ctx, const watch_def &d, int on)
    {
        JSValue o = JS_NewObject(ctx);
        JS_SetPropertyStr(ctx, o, "id", JS_NewInt32(ctx, d.id));
        JS_SetPropertyStr(ctx, o, "on", JS_NewString(ctx, ON_NAMES[on]));
        JS_SetPropertyStr(ctx, o, "reflex", JS_NewString(ctx, REFLEX_NAMES[d.reflex]));
        JS_SetPropertyStr(ctx, o, "name", JS_NewString(ctx, d.name));
        JS_SetPropertyStr(ctx, o, "handler", JS_NewBool(ctx, d.has_handler));
        JSValue c = JS_NewObject(ctx);
        JS_SetPropertyStr(ctx, c, "regs", JS_NewBool(ctx, d.regs));
        JS_SetPropertyStr(ctx, c, "fault", JS_NewBool(ctx, d.fault));
        JS_SetPropertyStr(ctx, c, "stack", JS_NewUint32(ctx, d.stack));
        JSValue mem = JS_NewArray(ctx);
        for (int i = 0; i < d.n_mem; i++) {
            JSValue pair = JS_NewArray(ctx);
            JS_SetPropertyUint32(ctx, pair, 0, JS_NewUint32(ctx, d.mem[i].addr));
            JS_SetPropertyUint32(ctx, pair, 1, JS_NewUint32(ctx, d.mem[i].len));
            JS_SetPropertyUint32(ctx, mem, i, pair);
        }
        JS_SetPropertyStr(ctx, c, "mem", mem);
        JS_SetPropertyStr(ctx, o, "capture", c);
        return o;
    }

    JSValue new_watch(JSContext *ctx, JSValueConst spec, JSValueConst handler)
    {
        if (!JS_IsObject(spec)) {
            return JS_ThrowTypeError(ctx, "watch: expected {on: 'fault' | 'halt' | 'lockup' | 'reset', ...}");
        }
        if (!JS_IsUndefined(handler) && !JS_IsNull(handler) && !JS_IsFunction(ctx, handler)) {
            return JS_ThrowTypeError(ctx, "watch: the handler must be a function");
        }
        watch_def d = {};
        char buf[24];
        bool given = false;
        if (opt_str(ctx, spec, "on", buf, sizeof(buf), &given)) {
            return JS_EXCEPTION;
        }
        int on = given ? index_of(ON_NAMES, WATCHES, buf) : -1;
        if (on < 0) {
            return JS_ThrowRangeError(ctx, "watch: on must be 'fault', 'halt', 'lockup' or 'reset'");
        }
        if (opt_str(ctx, spec, "reflex", buf, sizeof(buf), &given)) {
            return JS_EXCEPTION;
        }
        int reflex = given ? index_of(REFLEX_NAMES, 3, buf) : (on == ON_RESET ? REFLEX_NONE : REFLEX_HALT);
        if (reflex < 0) {
            return JS_ThrowRangeError(ctx, "watch: reflex must be 'halt', 'none' or 'reset'");
        }
        if (on == ON_RESET && reflex == REFLEX_RESET) {
            return JS_ThrowRangeError(ctx, "watch: a reset watch can't reset; use reflex 'none' or 'halt'");
        }
        if (reflex == REFLEX_RESET && !g.cfg.perms.reset) {
            JSValue e = target_error(ctx, ESP_ERR_NOT_ALLOWED, "NOT_ALLOWED", "watch");
            if (!JS_IsException(e)) {
                JS_SetPropertyStr(ctx, e, "message",
                                  JS_NewString(ctx, "watch: NOT_ALLOWED, reflex 'reset' needs a session started with --allow-reset"));
            }
            return JS_Throw(ctx, e);
        }
        d.reflex = reflex;
        if (opt_str(ctx, spec, "name", d.name, sizeof(d.name), &given)) {
            return JS_EXCEPTION;
        }
        if (opt_u32(ctx, spec, "handlerTimeoutMs", HANDLER_TIMEOUT_MS, &d.handler_timeout_ms)) {
            return JS_EXCEPTION;
        }
        if (d.handler_timeout_ms == 0 || d.handler_timeout_ms > HANDLER_TIMEOUT_MAX) {
            return JS_ThrowRangeError(ctx, "watch: handlerTimeoutMs must be 1-%u", (unsigned)HANDLER_TIMEOUT_MAX);
        }

        JSValue cap = JS_GetPropertyStr(ctx, spec, "capture");
        if (JS_IsException(cap)) {
            return cap;
        }
        uint32_t stack = 0;
        JSValue mem = JS_IsObject(cap) ? JS_GetPropertyStr(ctx, cap, "mem") : JS_UNDEFINED;
        int bad = (!JS_IsUndefined(cap) && !JS_IsObject(cap)) || opt_bool(ctx, cap, "regs", true, &d.regs) ||
                  opt_bool(ctx, cap, "fault", on == ON_FAULT || on == ON_LOCKUP, &d.fault) ||
                  opt_u32(ctx, cap, "stack", on == ON_RESET ? 0 : STACK_DEFAULT, &stack) || JS_IsException(mem) ||
                  parse_mem(ctx, mem, d);
        JS_FreeValue(ctx, mem);
        JS_FreeValue(ctx, cap);
        if (bad) {
            if (!JS_HasException(ctx)) {
                JS_ThrowTypeError(ctx, "watch: capture must be an object");
            }
            return JS_EXCEPTION;
        }
        if ((stack & 3) || stack > RANGE_MAX) {
            return JS_ThrowRangeError(ctx, "watch: capture.stack must be a multiple of 4, at most %u", (unsigned)RANGE_MAX);
        }
        d.stack = stack;
        size_t total = d.stack;
        for (int i = 0; i < d.n_mem; i++) {
            total += d.mem[i].len;
        }
        if (total > CAPTURE_DATA_MAX) {
            return JS_ThrowRangeError(ctx, "watch: capture.stack and capture.mem add up to %u bytes, at most %u", (unsigned)total,
                                      (unsigned)CAPTURE_DATA_MAX);
        }

        d.used = true;
        d.has_handler = JS_IsFunction(ctx, handler);
        if (++g.watch_ids == 0) {
            g.watch_ids = 1;
        }
        d.id = g.watch_ids;
        xSemaphoreTake(g.watch_lock, portMAX_DELAY);
        bool taken = g.watches[on].used;
        uint16_t other = g.watches[on].id;
        if (!taken) {
            g.watches[on] = d;
        }
        xSemaphoreGive(g.watch_lock);
        if (taken) {
            char msg[96];
            snprintf(msg, sizeof(msg), "watch: BUSY, there's already a %s watch (id %u); cancel() it first", ON_NAMES[on], other);
            return busy_error(ctx, msg);
        }
        JS_FreeValue(ctx, g.watch_fn[on]);
        g.watch_fn[on] = d.has_handler ? JS_DupValue(ctx, handler) : JS_UNDEFINED;
        g.watch_gen++;

        JSValue o = describe(ctx, d, on);
        JSValue data[2] = {JS_NewInt32(ctx, on), JS_NewInt32(ctx, d.id)};
        JS_SetPropertyStr(ctx, o, "cancel", JS_NewCFunctionData(ctx, js_cancel, 0, 0, 2, data));
        return o;
    }

    JSValue list_watches(JSContext *ctx)
    {
        JSValue a = JS_NewArray(ctx);
        uint32_t n = 0;
        for (int on = 0; on < WATCHES; on++) {
            if (g.watches[on].used) { // the JS task is the only writer
                JS_SetPropertyUint32(ctx, a, n++, describe(ctx, g.watches[on], on));
            }
        }
        return a;
    }

    void clear_watches_js(JSContext *)
    {
        xSemaphoreTake(g.watch_lock, portMAX_DELAY);
        for (auto &d : g.watches) {
            d = {};
        }
        xSemaphoreGive(g.watch_lock);
        g.watch_gen++;
    }

    static void set_hex(JSContext *ctx, JSValueConst o, const char *name, uint32_t v)
    {
        JS_SetPropertyStr(ctx, o, name, JS_NewUint32(ctx, v));
    }

    static JSValue parse_capture(JSContext *ctx, JSValue ev, const capture_slot &c)
    {
        static const char *const names[] = {"r0", "r1", "r2", "r3", "r4", "r5", "r6", "r7", "r8", "r9", "r10", "r11",
                                            "r12", "sp", "lr", "pc", "xpsr", "msp", "psp", "", "special"};
        const uint8_t *p = c.data, *end = c.data + c.len;
        JSValue mem = JS_NewArray(ctx);
        uint32_t n_mem = 0;
        for (const uint8_t *s = p + 16; s + 8 <= end;) {
            uint8_t type = s[0], kind = s[1];
            uint32_t len = get32(s + 4);
            const uint8_t *b = s + 8;
            if (b + len > end) {
                break;
            }
            if (type == SEC_REGS) {
                JSValue regs = JS_NewObject(ctx);
                for (uint32_t i = 0; i + 8 <= len; i += 8) {
                    uint8_t id = b[i];
                    if (!b[i + 1] || id >= sizeof(names) / sizeof(names[0])) {
                        continue;
                    }
                    uint32_t v = get32(b + i + 4);
                    if (id == 20) {
                        set_hex(ctx, regs, "primask", v & 0xff);
                        set_hex(ctx, regs, "basepri", (v >> 8) & 0xff);
                        set_hex(ctx, regs, "faultmask", (v >> 16) & 0xff);
                        set_hex(ctx, regs, "control", v >> 24);
                    } else {
                        set_hex(ctx, regs, names[id], v);
                    }
                }
                JS_SetPropertyStr(ctx, ev, "regs", regs);
            } else if (type == SEC_FAULT && len >= 32) {
                JSValue f = JS_NewObject(ctx);
                uint32_t valid = get32(b);
                for (int i = 0; i < 7; i++) {
                    if (valid & (1u << i)) {
                        set_hex(ctx, f, FAULT_NAMES[i], get32(b + 4 + 4 * i));
                    }
                }
                JS_SetPropertyStr(ctx, ev, "fault", f);
            } else if (type == SEC_MEM && len >= 8) {
                JSValue m = JS_NewObject(ctx);
                set_hex(ctx, m, "addr", get32(b));
                JS_SetPropertyStr(ctx, m, "data", JS_NewUint8ArrayCopy(ctx, b + 8, len - 8));
                if (kind == MEM_STACK) {
                    JS_SetPropertyStr(ctx, ev, "stack", m);
                } else {
                    JS_SetPropertyUint32(ctx, mem, n_mem++, m);
                }
            }
            s = b + ((len + 3) & ~3u);
        }
        JS_SetPropertyStr(ctx, ev, "mem", mem);
        return ev;
    }

    static void send_verdict(const fire_msg &m, uint8_t v, uint8_t status)
    {
        if (!m.held) {
            return;
        }
        req r = {};
        r.op = OP_WATCH_ACTION;
        r.slot = SYNC;
        r.addr = m.on;
        r.a = v;
        r.b = m.fire;
        r.timeout_ms = status;
        if (xQueueSend(g.req_q, &r, pdMS_TO_TICKS(50)) != pdTRUE) {
            ESP_LOGW(TAG, "verdict lost, the owner's timeout holds the target"); // owner times out to hold
        }
    }

    static void handler_failed(JSContext *ctx, JSValueConst e, const fire_msg &m)
    {
        char what[40], json[EVENT_JSON_MAX + 1];
        snprintf(what, sizeof(what), "watch %u handler", m.id);
        error_json(ctx, e, what, json, sizeof(json));
        emit_event("error", json, m.capture, true);
    }

    static uint8_t verdict_of(JSContext *ctx, JSValueConst v, uint8_t *status)
    {
        *status = H_OK;
        if (JS_IsUndefined(v)) {
            return V_HOLD;
        }
        const char *s = JS_IsString(v) ? JS_ToCString(ctx, v) : nullptr;
        uint8_t out = V_HOLD;
        if (s && strcmp(s, "resume") == 0) {
            out = V_RESUME;
        } else if (s && strcmp(s, "reset") == 0) {
            out = V_RESET;
        } else if (!s || strcmp(s, "hold") != 0) {
            *status = H_BAD_VALUE;
        }
        JS_FreeCString(ctx, s);
        return out;
    }

    static void send_data(JSContext *ctx, JSValueConst data, const fire_msg &m)
    {
        if (!m.incident || JS_IsUndefined(data)) {
            return;
        }
        if (!note_claim(ctx, data, "watch handler data")) {
            JSValue e = JS_GetException(ctx);
            handler_failed(ctx, e, m);
            JS_FreeValue(ctx, e);
            return;
        }
        req r = {};
        r.op = OP_INCIDENT_DATA;
        r.slot = SYNC;
        r.addr = m.incident;
        if (xQueueSend(g.req_q, &r, pdMS_TO_TICKS(50)) != pdTRUE) {
            g.note_busy = false;
            ESP_LOGW(TAG, "incident data lost, the owner's queue is full");
        }
    }

    static uint8_t take_result(JSContext *ctx, JSValueConst v, const fire_msg &m, uint8_t *status)
    {
        if (!JS_IsObject(v) || JS_IsArray(v) || JS_IsFunction(ctx, v)) {
            return verdict_of(ctx, v, status);
        }
        JSValue data = JS_GetPropertyStr(ctx, v, "data"); // getters may throw: then hold, as for a bad value
        if (JS_IsException(data)) {
            drop_exception(ctx);
        } else {
            send_data(ctx, data, m);
        }
        JS_FreeValue(ctx, data);
        JSValue action = JS_GetPropertyStr(ctx, v, "action");
        if (JS_IsException(action)) {
            drop_exception(ctx);
            *status = H_BAD_VALUE;
            return V_HOLD;
        }
        uint8_t out = verdict_of(ctx, action, status);
        JS_FreeValue(ctx, action);
        return out;
    }

    static JSValue on_handler_settled(JSContext *ctx, JSValueConst, int argc, JSValueConst *argv, int magic, JSValueConst *data)
    {
        fire_msg m = {};
        size_t len = 0;
        uint8_t *raw = JS_GetArrayBuffer(ctx, &len, data[0]);
        if (!raw || len != sizeof(m)) {
            return JS_UNDEFINED;
        }
        memcpy(&m, raw, sizeof(m));
        JSValueConst v = argc ? argv[0] : JS_UNDEFINED;
        if (magic) {
            if (ends_runtime(v)) {
                return JS_UNDEFINED; // the owner sees the runtime end
            }
            handler_failed(ctx, v, m);
            send_verdict(m, V_HOLD, H_THREW);
            return JS_UNDEFINED;
        }
        uint8_t status;
        uint8_t verdict = take_result(ctx, v, m, &status);
        send_verdict(m, verdict, status);
        return JS_UNDEFINED;
    }

    void watch_cb(JSContext *ctx, void *arg)
    {
        fire_msg m = *static_cast<fire_msg *>(arg);
        heap_caps_free(arg);
        if (loop_stopping()) { // no handler now; the runtime's end settles a held wait
            if (m.slot >= 0) {
                g.caps[m.slot].js_busy = false;
            }
            return;
        }

        JSValue ev = JS_NewObject(ctx);
        JS_SetPropertyStr(ctx, ev, "watch", JS_NewInt32(ctx, m.id));
        JS_SetPropertyStr(ctx, ev, "on", JS_NewString(ctx, ON_NAMES[m.on]));
        JS_SetPropertyStr(ctx, ev, "name", JS_NewString(ctx, m.name));
        JS_SetPropertyStr(ctx, ev, "cause", JS_NewString(ctx, m.cause));
        JS_SetPropertyStr(ctx, ev, "reflex", JS_NewString(ctx, REFLEX_NAMES[m.reflex]));
        JS_SetPropertyStr(ctx, ev, "halted", JS_NewBool(ctx, m.halted));
        JS_SetPropertyStr(ctx, ev, "held", JS_NewBool(ctx, m.held));
        JS_SetPropertyStr(ctx, ev, "pc", JS_NewUint32(ctx, m.pc));
        JS_SetPropertyStr(ctx, ev, "captureId", JS_NewUint32(ctx, m.capture));
        JS_SetPropertyStr(ctx, ev, "incidentId", JS_NewUint32(ctx, m.incident));
        if (m.slot >= 0) {
            capture_slot &c = g.caps[m.slot];
            if (c.id == m.capture) {
                ev = parse_capture(ctx, ev, c);
            }
            c.js_busy = false;
        }

        JSValue fn = g.watches[m.on].used && g.watches[m.on].id == m.id ? JS_DupValue(ctx, g.watch_fn[m.on]) : JS_UNDEFINED;
        if (!JS_IsFunction(ctx, fn)) { // cancelled meanwhile
            JS_FreeValue(ctx, fn);
            JS_FreeValue(ctx, ev);
            send_verdict(m, V_HOLD, H_NONE);
            return;
        }
        JSValue r = JS_Call(ctx, fn, JS_UNDEFINED, 1, &ev);
        JS_FreeValue(ctx, fn);
        JS_FreeValue(ctx, ev);
        if (JS_IsException(r)) {
            JSValue e = JS_GetException(ctx);
            if (!ends_runtime(e)) {
                handler_failed(ctx, e, m);
                send_verdict(m, V_HOLD, H_THREW);
            }
            JS_FreeValue(ctx, e);
            return;
        }
        if (!JS_IsPromise(r)) {
            uint8_t status;
            uint8_t verdict = take_result(ctx, r, m, &status);
            JS_FreeValue(ctx, r);
            send_verdict(m, verdict, status);
            return;
        }
        JSValue data = JS_NewArrayBufferCopy(ctx, reinterpret_cast<const uint8_t *>(&m), sizeof(m));
        JSValue ok = JS_NewCFunctionData(ctx, on_handler_settled, 1, 0, 1, &data);
        JSValue bad = JS_NewCFunctionData(ctx, on_handler_settled, 1, 1, 1, &data);
        // Handlers that failed to allocate would be taken as none: no verdict, ever
        JSValue p = JS_IsException(data) || JS_IsException(ok) || JS_IsException(bad) ? JS_EXCEPTION
                                                                                      : JS_PromiseThen(ctx, r, ok, bad);
        if (JS_IsException(p)) {
            JS_FreeValue(ctx, JS_GetException(ctx));
            send_verdict(m, V_HOLD, H_THREW);
        }
        JS_FreeValue(ctx, p);
        JS_FreeValue(ctx, ok);
        JS_FreeValue(ctx, bad);
        JS_FreeValue(ctx, data);
        JS_FreeValue(ctx, r);
    }

    static const char *fault_name(uint32_t ipsr)
    {
        switch (ipsr) {
        case 3:
            return "HardFault";
        case 4:
            return "MemManage";
        case 5:
            return "BusFault";
        case 6:
            return "UsageFault";
        case 7:
            return "SecureFault";
        default:
            return "exception";
        }
    }

    static void emit_stop(const char *cause, bool halted, uint32_t pc)
    {
        char json[96];
        if (halted) {
            snprintf(json, sizeof(json), "{\"cause\":\"%s\",\"pc\":\"0x%08lx\"}", cause, (unsigned long)pc);
        } else {
            snprintf(json, sizeof(json), "{\"cause\":\"%s\"}", cause);
        }
        emit_event("stop", json, 0, false);
    }

    static void emit_target(const char *state, esp_err_t err)
    {
        char json[96];
        if (err != ESP_OK) {
            snprintf(json, sizeof(json), "{\"state\":\"%s\",\"error\":\"%s\"}", state, esp_err_to_name(err));
        } else {
            snprintf(json, sizeof(json), "{\"state\":\"%s\"}", state);
        }
        emit_event("target", json, 0, false);
    }

    static void emit_watch(const fire_msg &m, const char *action, const char *handler, bool throttled, const char *note)
    {
        char json[EVENT_JSON_MAX + 1];
        size_t n = snprintf(json, sizeof(json), "{\"watch\":%u,\"name\":", m.id);
        n += json_str(json + n, sizeof(json) - n, m.name);
        n += snprintf(json + n, sizeof(json) - n,
                      ",\"on\":\"%s\",\"cause\":\"%s\",\"reflex\":\"%s\",\"action\":\"%s\",\"handler\":\"%s\"", ON_NAMES[m.on],
                      m.cause, REFLEX_NAMES[m.reflex], action, handler);
        if (m.halted) {
            n += snprintf(json + n, sizeof(json) - n, ",\"pc\":\"0x%08lx\"", (unsigned long)m.pc);
        }
        if (throttled) {
            n += snprintf(json + n, sizeof(json) - n, ",\"throttled\":true");
        }
        if (m.incident) {
            n += snprintf(json + n, sizeof(json) - n, ",\"incident\":%lu", (unsigned long)m.incident);
        }
        if (note) {
            n += snprintf(json + n, sizeof(json) - n, ",\"note\":");
            n += json_str(json + n, sizeof(json) - n - 1, note);
        }
        snprintf(json + n, sizeof(json) - n, "}");
        emit_event("watch", json, m.capture, false);
        if (m.incident && g.cfg.incidents) {
            char out[160];
            size_t k = snprintf(out, sizeof(out), "{\"action\":\"%s\",\"handler\":\"%s\",\"throttled\":%s,\"note\":", action,
                                handler, throttled ? "true" : "false");
            k += json_str(out + k, sizeof(out) - k - 1, note ? note : "");
            k += strlcpy(out + k, "}", sizeof(out) - k);
            g.cfg.incidents->add(m.incident, incident_part::outcome, out, std::min(k, sizeof(out) - 1));
        }
    }

    static uint32_t open_incident(const fire_msg &m, const char *action)
    {
        if (!g.cfg.incidents) {
            return 0;
        }
        char cause[320];
        size_t n = snprintf(cause, sizeof(cause), "{\"kind\":\"watch\",\"watch\":%u,\"name\":", m.id);
        n = std::min(n, sizeof(cause) - 1);
        n += json_str(cause + n, sizeof(cause) - n, m.name);
        int w = snprintf(cause + n, sizeof(cause) - n,
                         ",\"on\":\"%s\",\"cause\":\"%s\",\"reflex\":\"%s\",\"first_action\":\"%s\",\"halted\":%s,\"fire\":%lu",
                         ON_NAMES[m.on], m.cause, REFLEX_NAMES[m.reflex], action, m.halted ? "true" : "false", (unsigned long)m.fire);
        if (w > 0) {
            n = std::min(n + (size_t)w, sizeof(cause) - 1);
        }
        if (m.halted) {
            w = snprintf(cause + n, sizeof(cause) - n, ",\"pc\":\"0x%08lx\"", (unsigned long)m.pc);
            if (w > 0) {
                n = std::min(n + (size_t)w, sizeof(cause) - 1);
            }
        }
        strlcpy(cause + n, "}", sizeof(cause) - n);
        const capture_slot *c = m.slot >= 0 && g.caps[m.slot].id == m.capture ? &g.caps[m.slot] : nullptr;
        incident_open o = {true, m.id, m.name, cause, c ? c->data : nullptr, c ? c->len : 0};
        return g.cfg.incidents->open(o);
    }

    static size_t begin_sec(uint8_t *p, size_t at, uint8_t type, uint8_t kind)
    {
        p[at] = type;
        p[at + 1] = kind;
        p[at + 2] = p[at + 3] = 0;
        put32(p + at + 4, 0);
        return at + 8;
    }

    static size_t end_sec(uint8_t *p, size_t start, size_t body_end)
    {
        uint32_t len = body_end - start;
        put32(p + start - 4, len);
        size_t padded = start + ((len + 3) & ~3u);
        memset(p + body_end, 0, padded - body_end);
        return padded;
    }

    // Reads what it can: a stack near the top of RAM runs past its end, and
    // the chunk that crosses it would otherwise lose its readable part too.
    static size_t read_into(js_target &t, uint32_t addr, uint32_t len, uint8_t *out)
    {
        size_t done = 0;
        while (done < len) {
            uint32_t n = std::min<uint32_t>(READ_CHUNK, len - done);
            if (t.read_mem(addr + done, out + done, n) != ESP_OK) {
                while (done + 4 <= len && t.read_mem(addr + done, out + done, 4) == ESP_OK) {
                    done += 4; // addr is word aligned: up to the first word that faults
                }
                break;
            }
            done += n;
        }
        return done;
    }

    static uint32_t capture(const watch_def &d, fire_msg &m, js_target &t, uint32_t dfsr, int *slot_out)
    {
        engine &e = g.eng;
        int slot = -1;
        for (int i = 0; i < CAPTURES && slot < 0; i++) {
            int idx = (e.next_slot + i) % CAPTURES;
            if (!g.caps[idx].js_busy) {
                slot = idx;
            }
        }
        *slot_out = slot;
        if (slot < 0) {
            return 0;
        }
        e.next_slot = slot + 1;
        capture_slot &c = g.caps[slot];
        c.id = 0; // invalid while it's rewritten
        uint8_t *p = c.data;
        size_t at = 16;

        uint32_t regs[21] = {};
        bool ok[21] = {};
        if (m.halted) {
            for (uint8_t id : REG_IDS) {
                ok[id] = t.read_reg(id, &regs[id]) == ESP_OK;
            }
            m.pc = ok[15] ? regs[15] : 0;
            if (d.regs) {
                size_t s = begin_sec(p, at, SEC_REGS, 0);
                size_t b = s;
                for (uint8_t id : REG_IDS) {
                    p[b] = id;
                    p[b + 1] = ok[id];
                    p[b + 2] = p[b + 3] = 0;
                    put32(p + b + 4, regs[id]);
                    b += 8;
                }
                at = end_sec(p, s, b);
            }
        }
        if (d.fault && g.cfg.fault_regs) {
            size_t s = begin_sec(p, at, SEC_FAULT, 0);
            uint32_t valid = 0;
            for (int i = 0; i < 7; i++) {
                uint32_t v = 0;
                if (t.read32(FAULT_ADDRS[i], &v) == ESP_OK) {
                    valid |= 1u << i;
                }
                put32(p + s + 4 + 4 * i, v);
            }
            put32(p + s, valid);
            at = end_sec(p, s, s + 32);
        }
        if (d.stack && m.halted && ok[13]) {
            uint32_t ipsr = regs[16] & 0x1ff, lr = regs[14];
            uint32_t sp = regs[13];
            if (ipsr != 0 && (lr & 0xFFFFFF00) == 0xFFFFFF00) { // in a handler: the frame is where EXC_RETURN says
                bool psp = lr & 4;
                if ((psp && ok[18]) || (!psp && ok[17])) {
                    sp = psp ? regs[18] : regs[17];
                }
            }
            sp &= ~3u;
            size_t s = begin_sec(p, at, SEC_MEM, MEM_STACK);
            put32(p + s, sp);
            put32(p + s + 4, d.stack);
            size_t got = read_into(t, sp, d.stack, p + s + 8);
            at = end_sec(p, s, s + 8 + got);
        }
        for (int i = 0; i < d.n_mem; i++) {
            size_t s = begin_sec(p, at, SEC_MEM, MEM_RANGE);
            put32(p + s, d.mem[i].addr);
            put32(p + s + 4, d.mem[i].len);
            size_t got = read_into(t, d.mem[i].addr, d.mem[i].len, p + s + 8);
            at = end_sec(p, s, s + 8 + got);
        }
        {
            size_t s = begin_sec(p, at, SEC_STOP, 0);
            put32(p + s, dfsr);
            put32(p + s + 4, m.halted);
            uint64_t us = now_us();
            memcpy(p + s + 8, &us, 8);
            memset(p + s + 16, 0, 16);
            strlcpy(reinterpret_cast<char *>(p + s + 16), m.cause, 16);
            at = end_sec(p, s, s + 32);
        }

        if (++e.captures == 0) {
            e.captures = 1;
        }
        memcpy(p, "SICP", 4);
        p[4] = 1;
        p[5] = m.on;
        p[6] = m.id & 0xff;
        p[7] = m.id >> 8;
        put32(p + 8, e.captures);
        put32(p + 12, at);
        c.len = at;
        c.id = e.captures;
        return c.id;
    }

    static const char *handler_text(uint8_t status)
    {
        switch (status) {
        case H_OK:
            return "ok";
        case H_THREW:
            return "threw";
        case H_BAD_VALUE:
            return "returned something other than 'hold', 'resume', 'reset' or {action, data}";
        default:
            return "none";
        }
    }

    static const char *apply(uint8_t v, js_target &t, const char **note)
    {
        engine &e = g.eng;
        switch (v) {
        case V_RESUME:
            t.clear_stop_cause(); // the stop is dealt with (see watch_poll)
            if (t.resume() == ESP_OK) {
                e.halted = false;
                return "resume";
            }
            *note = "resume failed";
            return "hold";
        case V_RESET:
            if (!g.cfg.perms.reset) {
                *note = "reset needs a session started with --allow-reset";
                return "hold";
            }
            t.clear_stop_cause();
            if (t.reset() == ESP_OK) {
                e.known = false;
                e.after_op = true;
                e.ignore_reset = true;
                e.vc_dirty = true;
                return "reset";
            }
            *note = "reset failed";
            return "hold";
        default:
            return "hold";
        }
    }

    static void fire(uint8_t on, const char *cause, bool halted, bool stopped, uint32_t dfsr, js_target &t) // stopped: by the event
    {
        engine &e = g.eng;
        e.throttle_until_us = 0; // an earlier throttled hold is over: this is a new stop
        watch_def d;
        xSemaphoreTake(g.watch_lock, portMAX_DELAY);
        d = g.watches[on];
        xSemaphoreGive(g.watch_lock);

        if (!d.used) {
            uint32_t pc = 0;
            if (halted) {
                t.read_reg(15, &pc);
            }
            emit_stop(cause, halted, pc);
            return;
        }

        int64_t now = now_us();
        if (now - e.window_us[on] > 1000000) {
            e.window_us[on] = now;
            e.window_n[on] = 0;
        }
        bool throttled = ++e.window_n[on] > BURST_MAX;

        fire_msg m = {};
        m.fire = ++e.fires;
        m.on = on;
        m.id = d.id;
        m.reflex = throttled ? (uint8_t)REFLEX_HALT : d.reflex;
        strlcpy(m.cause, cause, sizeof(m.cause));
        strlcpy(m.name, d.name, sizeof(m.name));
        bool halt_failed = false;
        if (m.reflex == REFLEX_HALT && !halted) {
            if (t.halt() == ESP_OK) {
                halted = true; // a lockup or a reset: stop it there
                e.halted = true;
            } else {
                halt_failed = true; // not held: the handler mustn't be told it is
            }
        }
        m.halted = halted;
        int slot = -1;
        m.capture = capture(d, m, t, dfsr, &slot);
        m.slot = m.capture ? slot : -1;

        const char *note = nullptr;
        const char *action = "hold";
        if (m.reflex == REFLEX_NONE) {
            action = "none";
            if (stopped) {
                action = apply(V_RESUME, t, &note);
            }
        } else if (m.reflex == REFLEX_RESET) {
            action = apply(V_RESET, t, &note);
        } else if (halt_failed) {
            action = "none";
            note = "the halt failed";
        }
        if (strcmp(action, "resume") == 0 || strcmp(action, "reset") == 0) {
            m.halted = false; // as the handler will find it, not as it was caught
        }

        m.incident = open_incident(m, action);

        bool call = d.has_handler && !throttled && g.alive;
        if (call) {
            m.held = m.reflex == REFLEX_HALT && strcmp(action, "hold") == 0;
            auto *copy = static_cast<fire_msg *>(heap_caps_malloc(sizeof(fire_msg), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
            if (m.slot >= 0) {
                g.caps[m.slot].js_busy = true;
            }
            xSemaphoreTake(g.loop_lock, portMAX_DELAY);
            if (copy) {
                *copy = m;
            }
            call = copy && g.loop && g.alive && qjs_esp_loop_post(g.loop, watch_cb, copy, pdMS_TO_TICKS(50)) == ESP_OK;
            xSemaphoreGive(g.loop_lock);
            if (!call) {
                heap_caps_free(copy);
                if (m.slot >= 0) {
                    g.caps[m.slot].js_busy = false;
                }
                m.held = false;
                note = note ? note : "the script runtime didn't take the handler";
            }
        }
        if (call && m.held) {
            e.waiting = true;
            e.w = m;
            e.w_until_us = now_us() + d.handler_timeout_ms * 1000LL;
            return;
        }
        emit_watch(m, action, call ? "called" : "none", throttled, note);
        if (throttled && m.halted && strcmp(action, "hold") == 0 && g.cfg.throttle_hold_ms != 0) {
            // Unattended (a trap): a boot loop held here would stay stopped until someone looks.
            e.throttle_until_us = now_us() + g.cfg.throttle_hold_ms * 1000LL;
        }
    }

    // A throttled hold that has lasted session_config.throttle_hold_ms: let the core run.
    static void end_throttle_hold(js_target &t)
    {
        engine &e = g.eng;
        e.throttle_until_us = 0;
        if (!e.known || !e.halted) {
            return; // resumed or reset by something else meanwhile
        }
        const char *note = nullptr;
        const char *action = apply(V_RESUME, t, &note);
        const bool resumed = strcmp(action, "resume") == 0;
        ESP_LOGI(TAG, "throttle hold over: %s", resumed ? "resumed" : note);
        emit_target(resumed ? "resumed after a throttle hold" : "throttle hold over, resume failed", resumed ? ESP_OK : ESP_FAIL);
    }

    static void finish_wait(uint8_t v, const char *handler, js_target &t)
    {
        engine &e = g.eng;
        const char *note = nullptr;
        const char *action = apply(v, t, &note);
        e.waiting = false;
        e.wait_orphaned = false;
        emit_watch(e.w, action, handler, false, note);
    }

    void watch_verdict(const req &r, js_target &t)
    {
        engine &e = g.eng;
        if (!e.waiting || r.b != e.w.fire) {
            return; // late, or for a watch that doesn't hold the target
        }
        finish_wait(r.a, handler_text(r.timeout_ms), t);
    }

    static void classify(const core_state &st, js_target &t, uint8_t *on, const char **cause)
    {
        if (st.lockup) {
            *on = ON_LOCKUP;
            *cause = "lockup";
            return;
        }
        if (st.dfsr & DFSR_VCATCH) {
            uint32_t xpsr = 0;
            if (t.read_reg(16, &xpsr) != ESP_OK) {
                // IPSR unknown: a reset only if that's the one catch armed; a
                // fault otherwise (the one not to miss)
                const bool reset_only = (g.eng.vc & VC_CORERESET) != 0 && (g.eng.vc & VC_FAULTS) == 0;
                *on = reset_only ? ON_RESET : ON_FAULT;
                *cause = reset_only ? "reset" : "fault";
                return;
            }
            uint32_t ipsr = xpsr & 0x1ff;
            *on = ipsr == 0 ? ON_RESET : ON_FAULT;
            *cause = ipsr == 0 ? "reset" : fault_name(ipsr);
            return;
        }
        *on = ON_HALT;
        *cause = st.dfsr & DFSR_BKPT       ? "breakpoint"
                 : st.dfsr & DFSR_DWTTRAP  ? "watchpoint"
                 : st.dfsr & DFSR_EXTERNAL ? "external"
                                           : "halt request";
    }

    static uint32_t wanted_vc()
    {
        uint32_t vc = 0;
        xSemaphoreTake(g.watch_lock, portMAX_DELAY);
        if (g.watches[ON_FAULT].used) {
            vc |= VC_FAULTS;
        }
        if (g.watches[ON_RESET].used && g.watches[ON_RESET].reflex == REFLEX_HALT) {
            vc |= VC_CORERESET;
        }
        xSemaphoreGive(g.watch_lock);
        return vc;
    }

    void watch_poll(js_target &t)
    {
        engine &e = g.eng;
        int64_t now = now_us();
        uint32_t gen = g.watch_gen;
        if ((gen != e.gen || e.vc_dirty) && !e.lost) {
            uint32_t vc = wanted_vc();
            if (t.set_vector_catch(vc) == ESP_OK) {
                e.vc = vc;
                e.gen = gen;
                e.vc_dirty = false;
            }
        }
        if (e.waiting) {
            if (!g.alive || e.wait_orphaned) {
                finish_wait(V_HOLD, "runtime ended", t);
            } else if (now >= e.w_until_us) {
                finish_wait(V_HOLD, "timeout", t);
            }
            return;
        }
        if (e.throttle_until_us != 0 && now >= e.throttle_until_us) {
            end_throttle_hold(t);
        }
        if (now < e.next_poll_us) {
            return;
        }
        e.next_poll_us = now + POLL_US;
        if (!e.known && !e.after_op && result_pending()) {
            return; // the first look waits for the module's watches, or a stop found then is only a stop event
        }

        core_state st = {};
        esp_err_t ret = t.poll_core(&st);
        if (ret != ESP_OK) {
            e.known = false;
            e.after_op = false; // a stop found once it answers again isn't ours
            if (++e.fails >= LOST_AFTER && !e.lost) {
                e.lost = true;
                emit_target("not responding", ret);
                ESP_LOGW(TAG, "target not responding: %s", esp_err_to_name(ret));
            }
            e.next_poll_us = now + (e.lost ? LOST_POLL_US : POLL_US);
            return;
        }
        e.fails = 0;
        if (e.lost) {
            e.lost = false;
            e.vc_dirty = true;
            emit_target("responding", ESP_OK);
        }
        bool reset = st.reset_seen || e.reset_latch;
        e.reset_latch = false;
        if (!e.known) {
            // The first look, or the first since the target answered again: a core
            // halted by a vector catch, a BKPT, a DWT match or EDBGRQ stopped with
            // nobody watching (DFSR keeps the cause until it's cleared), e.g. a
            // fault while the probe restarted under a trap. Report it, or the target
            // stays stopped and the trap records nothing. After our own op (a call,
            // flash work, a reset) the stop is ours.
            const bool stop_cause = st.halted && (st.dfsr & DFSR_STOP_CAUSES) != 0;
            const bool ours = e.after_op;
            // A lockup found running (not halted) is one nobody saw either
            const bool found_lockup = st.lockup && !st.halted;
            const bool unseen = !e.after_op && (stop_cause || found_lockup);
            if (e.after_op && stop_cause) {
                t.clear_stop_cause(); // ours: a call or the flash algorithm ends at a BKPT
            }
            e.known = true;
            e.after_op = false;
            e.halted = st.halted && !unseen;
            e.lockup = st.lockup && !(unseen && found_lockup); // so the lockup below fires
            e.ignore_reset = false;
            if (!unseen) {
                if (e.halted && !ours && g.cfg.throttle_hold_ms != 0) {
                    // Halted by a request (a throttled hold before the probe restarted, a
                    // debugger): unattended, nobody would let it go. Same limit as a throttled hold.
                    e.throttle_until_us = now + g.cfg.throttle_hold_ms * 1000LL;
                    ESP_LOGI(TAG, "target found halted; letting it run in %lu ms", (unsigned long)g.cfg.throttle_hold_ms);
                }
                return;
            }
            ESP_LOGI(TAG, "target found stopped (DFSR 0x%lx), reporting it", (unsigned long)st.dfsr);
            reset = false; // S_RESET_ST from before we looked
        }
        if (reset && e.ignore_reset) {
            reset = false;
            e.ignore_reset = false;
        }

        if (st.halted && !e.halted) {
            e.halted = true;
            uint8_t on;
            const char *cause;
            classify(st, t, &on, &cause);
            // DFSR keeps the cause until the stop is dealt with (resumed or reset,
            // watch_before_op): if the probe restarts before that, its next first
            // look finds the stop again instead of a core that seems halted by hand.
            if (on == ON_RESET) {
                reset = false; // the same reset, caught at its vector
            }
            e.lockup = st.lockup;
            fire(on, cause, true, true, st.dfsr, t);
        } else if (!st.halted && e.halted) {
            e.halted = false; // resumed by something else (a DHCSR write)
            e.throttle_until_us = 0;
            t.clear_stop_cause();
        }
        if (!st.halted && st.lockup && !e.lockup) {
            e.lockup = true;
            fire(ON_LOCKUP, "lockup", false, false, 0, t);
        } else if (!st.lockup) {
            e.lockup = false;
        }
        if (reset && e.waiting) {
            // A stop's watch holds the target for its verdict (a firmware that
            // faults right after a reset shows both in one poll): firing now
            // would overwrite that wait. Report the reset once it's settled.
            e.reset_latch = true;
            reset = false;
        }
        if (reset) {
            bool watched;
            xSemaphoreTake(g.watch_lock, portMAX_DELAY);
            watched = g.watches[ON_RESET].used;
            xSemaphoreGive(g.watch_lock);
            if (watched) {
                fire(ON_RESET, "reset", e.halted, false, 0, t);
            } else {
                emit_target("reset", ESP_OK);
            }
        }
    }

    void watch_before_op(uint8_t op, uint32_t addr, js_target &t)
    {
        // An op that lets a stopped core go (or resets it) deals with the stop.
        const bool goes = op == OP_RESUME || op == OP_RESET || op == OP_REINIT || op == OP_CALL || op == OP_FLASH_WRITE ||
                          op == OP_FLASH_ERASE || ((op == OP_WRITE32 || op == OP_RMW32) && addr == DHCSR);
        if (goes) {
            g.eng.throttle_until_us = 0; // the script deals with the held core itself
        }
        if (goes && g.eng.known && g.eng.halted) {
            t.clear_stop_cause();
        }
    }

    void watch_after_op(uint8_t op, uint32_t addr, esp_err_t ret)
    {
        engine &e = g.eng;
        switch (op) {
        case OP_HALT:
            if (ret == ESP_OK) {
                e.halted = true;
            }
            break;
        case OP_RESUME:
            if (ret == ESP_OK) {
                e.halted = false;
            }
            break;
        case OP_RESET:
        case OP_REINIT:
            e.known = false;
            e.after_op = true;
            e.ignore_reset = true;
            e.vc_dirty = true;
            break;
        case OP_CALL:
        case OP_FLASH_ERASE:
        case OP_FLASH_WRITE:
            e.known = false;
            e.after_op = true;
            e.vc_dirty = true;
            break;
        case OP_WRITE32:
        case OP_RMW32:
            if (addr == DEMCR) {
                e.vc_dirty = true;
            } else if (addr == DHCSR) {
                e.known = false;
                e.after_op = true;
            }
            break;
        default:
            break;
        }
    }

    void watch_reset_engine()
    {
        g.eng = {};
        g.eng.vc_dirty = true; // clear what a previous debugger left
        for (auto &c : g.caps) {
            c.id = 0;
            c.len = 0;
            c.js_busy = false;
        }
    }

    void watch_disarm(js_target &t)
    {
        t.set_vector_catch(0);
        g.eng = {};
    }
} // namespace qjs_binder::priv

#endif
