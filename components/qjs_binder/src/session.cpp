#include <algorithm>
#include <cstdio>
#include <cstring>

#include <esp_heap_caps.h>
#include <esp_log.h>
#include <psa/crypto.h>

#include "host_priv.hpp"
#include "quickjs-libc.h"

#if CONFIG_SI_JS_ENABLE

namespace qjs_binder
{
    using namespace priv;

    static const char *TAG = "js_session";
    static constexpr uint32_t DEFAULT_WAIT_MS = 5000;
    static constexpr uint32_t MAX_WAIT_MS = 60000;
    static constexpr uint32_t START_MS = 2000;      // a new runtime takes its first post
    static constexpr uint32_t GRACE_MS = 2000;      // a stopped runtime gives the arena back
    static constexpr float EV_RATE = 20;            // console and emit events per second
    static constexpr float EV_BURST = 40;
    static constexpr size_t EMIT_NAME_MAX = 31;
    static constexpr int SERVE_BATCH = 16;
    static constexpr uint32_t CLAIMED = 1u << 31;    // want_token: the JS task is writing that result
    static constexpr uint32_t CLAIMED_WAIT_MS = 1000;

    static void *ps_alloc(size_t n)
    {
        return heap_caps_calloc(1, n, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    }

    bool priv::session_alloc()
    {
        g.watch_lock = xSemaphoreCreateMutex();
        g.ev_lock = xSemaphoreCreateMutex();
        g.events = static_cast<ev_slot *>(ps_alloc(EVENTS * sizeof(ev_slot)));
        g.res_value = static_cast<char *>(ps_alloc(RESULT_VALUE_MAX + 1));
        g.res_stack = static_cast<char *>(ps_alloc(RESULT_STACK_MAX + 1));
        g.note = static_cast<char *>(ps_alloc(INCIDENT_DATA_MAX + 1));
        bool ok = g.watch_lock && g.ev_lock && g.events && g.res_value && g.res_stack && g.note;
        for (auto &c : g.caps) {
            c.data = static_cast<uint8_t *>(ps_alloc(CAPTURE_SIZE));
            ok = ok && c.data;
        }
        return ok;
    }

    size_t priv::json_str(char *out, size_t cap, const char *s, size_t max_in)
    {
        if (cap < 3) {
            if (cap) {
                out[0] = '\0';
            }
            return 0;
        }
        size_t n = 0, i = 0;
        out[n++] = '"';
        for (; s && s[i] && i < max_in; i++) {
            unsigned char c = s[i];
            char esc[8];
            size_t m = 2;
            esc[0] = '\\';
            switch (c) {
            case '"':
            case '\\':
                esc[1] = c;
                break;
            case '\n':
                esc[1] = 'n';
                break;
            case '\r':
                esc[1] = 'r';
                break;
            case '\t':
                esc[1] = 't';
                break;
            default:
                if (c < 0x20) {
                    m = snprintf(esc, sizeof(esc), "\\u%04x", c);
                } else {
                    esc[0] = c;
                    m = 1;
                }
            }
            if (n + m + 2 > cap) {
                break;
            }
            memcpy(out + n, esc, m);
            n += m;
        }
        if (s && s[i] && i < max_in) { // cut short: don't leave half a UTF-8 character
            while (n > 1 && ((unsigned char)out[n - 1] & 0xC0) == 0x80) {
                n--;
            }
            if (n > 1 && ((unsigned char)out[n - 1] & 0xC0) == 0xC0) {
                n--;
            }
        }
        out[n++] = '"';
        out[n] = '\0';
        return n;
    }

    static size_t utf8_cut(const char *s, size_t len, size_t max)
    {
        if (len <= max) {
            return len;
        }
        size_t n = max;
        while (n > 0 && ((unsigned char)s[n] & 0xC0) == 0x80) {
            n--;
        }
        return n;
    }

    bool emit(const char *kind, const char *json, uint32_t capture_id, bool limited)
    {
        if (!g.events) {
            return false;
        }
        if (strlen(json) > EVENT_JSON_MAX) {
            json = "{\"truncated\":true}";
        }
        xSemaphoreTake(g.ev_lock, portMAX_DELAY);
        int64_t now = now_us();
        if (limited) {
            g.ev_tokens = std::min(EV_BURST, g.ev_tokens + (now - g.ev_refill_us) * EV_RATE / 1e6f);
            g.ev_refill_us = now;
            if (g.ev_tokens < 1) {
                g.ev_dropped++;
                xSemaphoreGive(g.ev_lock);
                return false;
            }
            g.ev_tokens -= 1;
        }
        uint32_t seq = ++g.ev_seq;
        ev_slot &e = g.events[seq % EVENTS];
        e.seq = seq;
        e.capture_id = capture_id;
        e.uptime_us = now;
        strlcpy(e.kind, kind, sizeof(e.kind));
        strlcpy(e.json, json, sizeof(e.json));
        xSemaphoreGive(g.ev_lock);
        if (g.cfg.notify) {
            g.cfg.notify(seq);
        }
        return true;
    }

    void priv::emit_event(const char *kind, const char *json, uint32_t capture_id, bool limited)
    {
        emit(kind, json, capture_id, limited);
    }

    void priv::session_console(int level, const char *line)
    {
        static const char *levels[] = {"log", "warn", "error"};
        char json[EVENT_JSON_MAX + 1];
        int n = snprintf(json, sizeof(json), "{\"level\":\"%s\",\"text\":", levels[level < 0 || level > 2 ? 0 : level]);
        n += json_str(json + n, sizeof(json) - n - 1, line);
        strlcpy(json + n, "}", sizeof(json) - n);
        emit("console", json, 0, true);
    }

    size_t priv::error_json(JSContext *ctx, JSValueConst e, const char *what, char *out, size_t cap)
    {
        const char *msg = JS_ToCString(ctx, e);
        JSValue stack = JS_IsError(e) ? JS_GetPropertyStr(ctx, e, "stack") : JS_UNDEFINED;
        const char *st = JS_IsString(stack) ? JS_ToCString(ctx, stack) : nullptr;
        auto room = [&](size_t n, size_t keep) { return cap > n + keep ? cap - n - keep : 0; }; // keep: what follows
        size_t n = strlcpy(out, "{\"what\":", cap);
        n += json_str(out + n, room(n, 64), what);
        n += strlcpy(out + n, ",\"message\":", room(n, 0));
        n += json_str(out + n, room(n, 16), msg ? msg : "(error)", 240);
        if (st && room(n, 32)) {
            n += strlcpy(out + n, ",\"stack\":", room(n, 0));
            n += json_str(out + n, room(n, 2), st);
        }
        n += strlcpy(out + n, "}", room(n, 0));
        JS_FreeCString(ctx, st);
        JS_FreeCString(ctx, msg);
        JS_FreeValue(ctx, stack);
        return n;
    }

    static void on_uncaught(JSContext *ctx, qjs_esp_uncaught_t what, JSValueConst err, void *)
    {
        char json[EVENT_JSON_MAX + 1];
        error_json(ctx, err, what == QJS_ESP_UNHANDLED_REJECTION ? "unhandled rejection" : "uncaught error", json, sizeof(json));
        emit("error", json, 0, true);
    }

    static JSValue replacer(JSContext *ctx, JSValueConst, int argc, JSValueConst *argv)
    {
        JSValueConst v = argc > 1 ? argv[1] : JS_UNDEFINED;
        if (JS_IsBigInt(v)) {
            return JS_ToString(ctx, v);
        }
        if (JS_IsError(v)) {
            return JS_ToString(ctx, v);
        }
        if (JS_GetTypedArrayType(v) >= 0) {
            int64_t len = 0;
            if (JS_GetLength(ctx, v, &len)) {
                return JS_EXCEPTION;
            }
            JSValue a = JS_NewArray(ctx);
            for (int64_t i = 0; i < len && !JS_IsException(a); i++) {
                JS_SetPropertyUint32(ctx, a, i, JS_GetPropertyUint32(ctx, v, i));
            }
            return a;
        }
        return JS_DupValue(ctx, v);
    }

    static size_t to_json(JSContext *ctx, JSValueConst v, char *out, size_t cap, bool *truncated)
    {
        JSValue fn = JS_NewCFunction(ctx, replacer, "replacer", 2);
        JSValue s = JS_JSONStringify(ctx, v, fn, JS_UNDEFINED);
        JS_FreeValue(ctx, fn);
        const char *str = nullptr;
        size_t len = 0;
        if (JS_IsException(s)) {
            JSValue e = JS_GetException(ctx);
            const char *msg = JS_ToCString(ctx, e);
            len = snprintf(out, cap, "\"(not JSON: %s)\"", msg ? msg : "?");
            JS_FreeCString(ctx, msg);
            JS_FreeValue(ctx, e);
            *truncated = false;
            return std::min(len, cap - 1);
        }
        if (JS_IsUndefined(s)) {
            str = "undefined";
            len = strlen(str);
        } else {
            str = JS_ToCStringLen(ctx, &len, s);
        }
        size_t n = str ? utf8_cut(str, len, cap - 1) : 0;
        if (str) {
            memcpy(out, str, n);
        }
        out[n] = '\0';
        *truncated = str && n < len;
        if (!JS_IsUndefined(s)) {
            JS_FreeCString(ctx, str);
        }
        JS_FreeValue(ctx, s);
        return n;
    }

    static void send_done_token(uint32_t token)
    {
        req r = {};
        r.op = OP_DONE;
        r.seq = token;
        if (xQueueSend(g.req_q, &r, pdMS_TO_TICKS(100)) != pdTRUE) {
            ESP_LOGE(TAG, "owner queue full, result notice lost"); // the owner's wait ends it
        }
    }

    static const char *outcome_name(script_outcome o)
    {
        static const char *names[] = {"ok", "error", "pending", "stopped"};
        return names[static_cast<int>(o)];
    }

    static bool claim_result(uint32_t token)
    {
        uint32_t expect = token; // once claimed, the owner waits for this token's OP_DONE
        return token != 0 && g.want_token.compare_exchange_strong(expect, token | CLAIMED);
    }

    static void late_result(uint32_t token, script_outcome o, const char *text)
    {
        char json[EVENT_JSON_MAX + 1];
        size_t n = snprintf(json, sizeof(json), "{\"token\":%lu,\"outcome\":\"%s\",\"text\":", (unsigned long)token, outcome_name(o));
        n += json_str(json + n, sizeof(json) - n - 1, text, 600);
        strlcpy(json + n, "}", sizeof(json) - n);
        emit("result", json, 0, false);
    }

    static void result_text(uint32_t token, script_outcome o, const char *error, const char *stack = "")
    {
        if (!claim_result(token)) {
            late_result(token, o, error);
            return;
        }
        g.res_outcome = o;
        g.res_truncated = false;
        g.res_value[0] = '\0';
        strlcpy(g.res_error, error, sizeof(g.res_error));
        strlcpy(g.res_stack, stack, RESULT_STACK_MAX + 1);
        send_done_token(token);
    }

    static void result_error(JSContext *ctx, uint32_t token, JSValueConst e)
    {
        const char *msg = JS_ToCString(ctx, e);
        JSValue stack = JS_IsError(e) ? JS_GetPropertyStr(ctx, e, "stack") : JS_UNDEFINED;
        const char *st = JS_IsString(stack) ? JS_ToCString(ctx, stack) : nullptr;
        result_text(token, script_outcome::error, msg ? msg : "(error)", st ? st : "");
        JS_FreeCString(ctx, st);
        JS_FreeCString(ctx, msg);
        JS_FreeValue(ctx, stack);
    }

    static void result_value(JSContext *ctx, uint32_t token, JSValueConst v)
    {
        if (!claim_result(token)) {
            char text[601];
            bool cut = false;
            to_json(ctx, v, text, sizeof(text), &cut);
            late_result(token, script_outcome::ok, text);
            return;
        }
        g.res_outcome = script_outcome::ok;
        to_json(ctx, v, g.res_value, RESULT_VALUE_MAX + 1, &g.res_truncated);
        g.res_error[0] = '\0';
        g.res_stack[0] = '\0';
        send_done_token(token);
    }

    static JSValue on_job_settled(JSContext *ctx, JSValueConst, int argc, JSValueConst *argv, int magic, JSValueConst *data)
    {
        uint32_t token = 0;
        int32_t is_eval = 0;
        JS_ToUint32(ctx, &token, data[0]);
        JS_ToInt32(ctx, &is_eval, data[1]);
        JSValueConst v = argc ? argv[0] : JS_UNDEFINED;
        if (magic) {
            if (!ends_runtime(v)) {
                g.module_failed = g.module_failed || !is_eval;
                result_error(ctx, token, v);
            }
        } else if (is_eval) {
            JSValue inner = JS_IsObject(v) ? JS_GetPropertyStr(ctx, v, "value") : JS_UNDEFINED; // async scripts give {value}
            result_value(ctx, token, inner);
            JS_FreeValue(ctx, inner);
        } else {
            result_text(token, script_outcome::ok, "");
        }
        return JS_UNDEFINED;
    }

    static void settle_job(JSContext *ctx, JSValue r, uint32_t token, bool is_eval)
    {
        if (JS_IsException(r)) {
            JSValue e = JS_GetException(ctx);
            if (!ends_runtime(e)) { // else the runtime's end answers
                g.module_failed = g.module_failed || !is_eval;
                result_error(ctx, token, e);
            }
            JS_FreeValue(ctx, e);
            return;
        }
        if (!JS_IsPromise(r)) {
            if (is_eval) {
                result_value(ctx, token, r);
            } else {
                result_text(token, script_outcome::ok, "");
            }
            JS_FreeValue(ctx, r);
            return;
        }
        JSValue data[2] = {JS_NewUint32(ctx, token), JS_NewInt32(ctx, is_eval)};
        JSValue ok = JS_NewCFunctionData(ctx, on_job_settled, 1, 0, 2, data);
        JSValue bad = JS_NewCFunctionData(ctx, on_job_settled, 1, 1, 2, data);
        JSValue p = JS_PromiseThen(ctx, r, ok, bad);
        if (JS_IsException(p)) {
            JS_FreeValue(ctx, JS_GetException(ctx));
            result_text(token, script_outcome::error, "out of memory");
        }
        JS_FreeValue(ctx, p);
        JS_FreeValue(ctx, ok);
        JS_FreeValue(ctx, bad);
        JS_FreeValue(ctx, r);
    }

    static JSValue eval_script(JSContext *ctx, const uint8_t *blob, size_t len)
    {
        if (len < QJS_ESP_BC_HDR_LEN || memcmp(blob, "QJBC", 4) || blob[4] != 1 || blob[5] || blob[6] || blob[7]) {
            return JS_ThrowTypeError(ctx, "not a bytecode container");
        }
        uint32_t bc_len = blob[8] | blob[9] << 8 | blob[10] << 16 | (uint32_t)blob[11] << 24;
        if (bc_len == 0 || bc_len != len - QJS_ESP_BC_HDR_LEN) {
            return JS_ThrowTypeError(ctx, "bytecode container: bad length");
        }
        JSValue obj = JS_ReadObject(ctx, blob + QJS_ESP_BC_HDR_LEN, bc_len, JS_READ_OBJ_BYTECODE);
        if (JS_IsException(obj)) {
            return obj;
        }
        if (JS_VALUE_GET_TAG(obj) != JS_TAG_FUNCTION_BYTECODE) {
            JS_FreeValue(ctx, obj);
            return JS_ThrowTypeError(ctx, "eval wants a script, this is a module");
        }
        return JS_EvalFunction(ctx, obj); // frees obj
    }

    static void start_job(JSContext *ctx, uint8_t kind, uint32_t token, uint8_t *blob, size_t len)
    {
        JSValue r = kind == FIRST_MODULE ? qjs_esp_bc_eval(ctx, blob, len, nullptr) : eval_script(ctx, blob, len);
        heap_caps_free(blob); // read into the runtime by now
        settle_job(ctx, r, token, kind == FIRST_EVAL);
    }

    struct eval_post {
        uint32_t token;
        uint8_t *blob;
        size_t len;
    };

    static void eval_work(JSContext *ctx, void *arg)
    {
        eval_post p = *static_cast<eval_post *>(arg);
        heap_caps_free(arg);
        start_job(ctx, FIRST_EVAL, p.token, p.blob, p.len);
    }

    void priv::run_session_runtime()
    {
        uint8_t kind = g.first_kind;
        uint32_t token = g.first_token;
        uint8_t *blob = g.first_blob;
        size_t len = g.first_len;
        g.first_kind = FIRST_NONE;
        g.first_blob = nullptr;

        JSRuntime *rt = qjs_esp_new_runtime_in(g.arena);
        if (!rt) {
            qjs_esp_arena_reset(g.arena); // leftovers are garbage
            rt = qjs_esp_new_runtime_in(g.arena);
        }
        JSContext *ctx = rt ? (js_std_init_handlers(rt), new_session_context(rt)) : nullptr;
        qjs_esp_loop_t *loop = ctx ? qjs_esp_loop_new(ctx) : nullptr;
        if (!loop) {
            heap_caps_free(blob);
            if (token) {
                result_text(token, script_outcome::stopped, "no room for the script runtime");
            }
            emit("runtime", "{\"reason\":\"no room for the script runtime\"}", 0, false);
            qjs_esp_arena_reset(g.arena);
            g.rt_ending = true;
            return;
        }
        JS_SetModuleLoaderFunc(rt, nullptr, refuse_module, nullptr);
        g.lim = {CONFIG_SI_JS_SLICE_MS, CONFIG_SI_JS_YIELD_MS};
        qjs_esp_loop_set_limits(loop, &g.lim);
        qjs_esp_loop_keep_alive(loop, true);
        qjs_esp_loop_set_error_handler(loop, on_uncaught, nullptr);
        xSemaphoreTake(g.loop_lock, portMAX_DELAY);
        g.loop = loop;
        g.alive = true;
        xSemaphoreGive(g.loop_lock);

        g.results = JS_UNDEFINED;
        g.on_progress = JS_UNDEFINED;
        g.xfer_busy = false;
        g.loading = false;
        g.hook_active = false;
        for (auto &fn : g.watch_fn) {
            fn = JS_UNDEFINED;
        }
        if (kind != FIRST_NONE) {
            start_job(ctx, kind, token, blob, len);
        }

        qjs_esp_stop_t why = qjs_esp_loop_run(loop);
        g.rt_ending = true;
        g.alive = false;
        bool wanted = g.stop_wanted.load();
        if (!wanted) {
            char json[96];
            snprintf(json, sizeof(json), "{\"reason\":\"%s\"}", stop_text(why));
            emit("runtime", json, 0, false);
            ESP_LOGW(TAG, "runtime ended: %s", stop_text(why));
        }
        uint32_t want = g.want_token.load();
        if (want && !(want & CLAIMED)) {
            result_text(want, script_outcome::stopped, wanted ? "script stopped" : stop_text(why));
        }
        clear_watches_js(ctx);

        xSemaphoreTake(g.loop_lock, portMAX_DELAY);
        qjs_esp_loop_free(loop); // runs posted work, which frees its args
        g.loop = nullptr;
        xSemaphoreGive(g.loop_lock);
        memset(g.slots, 0, sizeof(g.slots));
        for (auto &fn : g.watch_fn) {
            fn = JS_UNDEFINED; // freed with the arena
        }
        g.mod = nullptr;
        log_runtime_end("session runtime");
        qjs_esp_arena_reset(g.arena);
    }

    static JSValue js_emit(JSContext *ctx, JSValueConst, int argc, JSValueConst *argv)
    {
        size_t name_len = 0;
        const char *name = JS_ToCStringLen(ctx, &name_len, argv[0]);
        if (!name) {
            return JS_EXCEPTION;
        }
        if (name_len == 0 || name_len > EMIT_NAME_MAX) {
            JS_FreeCString(ctx, name);
            return JS_ThrowRangeError(ctx, "emit: the name must be 1-%u bytes", (unsigned)EMIT_NAME_MAX);
        }
        char json[EVENT_JSON_MAX + 2];
        size_t n = snprintf(json, sizeof(json), "{\"name\":");
        n += json_str(json + n, sizeof(json) - n, name);
        JS_FreeCString(ctx, name);
        if (argc > 1 && !JS_IsUndefined(argv[1])) {
            n += snprintf(json + n, sizeof(json) - n, ",\"data\":");
            bool cut = false;
            size_t m = to_json(ctx, argv[1], json + n, sizeof(json) - n - 1, &cut);
            if (cut || n + m + 1 > EVENT_JSON_MAX) {
                return JS_ThrowRangeError(ctx, "emit: data too big, the event has room for about %u bytes of JSON",
                                          (unsigned)(EVENT_JSON_MAX - n - 1));
            }
            n += m;
        }
        strlcpy(json + n, "}", sizeof(json) - n);
        return JS_NewBool(ctx, emit("emit", json, 0, true));
    }

    bool priv::note_claim(JSContext *ctx, JSValueConst data, const char *what)
    {
        bool expect = false;
        if (!g.note_busy.compare_exchange_strong(expect, true)) {
            JS_ThrowTypeError(ctx, "%s: busy, the last incident is still being recorded (await agent.incident())", what);
            return false;
        }
        g.note_len = 0;
        if (JS_IsUndefined(data)) {
            g.note[0] = '\0';
            return true;
        }
        bool cut = false;
        size_t n = to_json(ctx, data, g.note, INCIDENT_DATA_MAX + 1, &cut);
        if (cut) {
            g.note_busy = false;
            JS_ThrowRangeError(ctx, "%s: over %u bytes of JSON", what, (unsigned)INCIDENT_DATA_MAX);
            return false;
        }
        g.note_len = strcmp(g.note, "undefined") == 0 ? 0 : n;
        return true;
    }

    static JSValue js_incident(JSContext *ctx, JSValueConst, int argc, JSValueConst *argv)
    {
        size_t name_len = 0;
        const char *name = JS_ToCStringLen(ctx, &name_len, argv[0]);
        if (!name) {
            return JS_EXCEPTION;
        }
        if (name_len == 0 || name_len >= sizeof(g.note_name)) {
            JS_FreeCString(ctx, name);
            return JS_ThrowRangeError(ctx, "agent.incident: the name must be 1-%u bytes", (unsigned)sizeof(g.note_name) - 1);
        }
        if (!note_claim(ctx, argc > 1 ? argv[1] : JS_UNDEFINED, "agent.incident")) {
            JS_FreeCString(ctx, name);
            return JS_EXCEPTION;
        }
        strlcpy(g.note_name, name, sizeof(g.note_name));
        JS_FreeCString(ctx, name);
        req r = {};
        r.op = OP_INCIDENT;
        JSValue p = async_call(ctx, r);
        if (JS_IsException(p)) {
            g.note_busy = false;
        }
        return p;
    }

    void priv::incident_from_script(const req &r)
    {
        uint32_t id = 0;
        if (g.cfg.incidents) {
            char cause[96];
            size_t n = strlcpy(cause, "{\"kind\":\"script\",\"name\":", sizeof(cause));
            n += json_str(cause + n, sizeof(cause) - n - 1, g.note_name);
            strlcpy(cause + n, "}", sizeof(cause) - n);
            incident_open o = {false, 0, g.note_name, cause, nullptr, 0};
            id = g.cfg.incidents->open(o);
            if (id && g.note_len) {
                g.cfg.incidents->add(id, incident_part::data, g.note, g.note_len);
            }
        }
        char json[EVENT_JSON_MAX + 1];
        size_t n = strlcpy(json, "{\"name\":", sizeof(json));
        n += json_str(json + n, sizeof(json) - n, g.note_name);
        n += snprintf(json + n, sizeof(json) - n, ",\"incident\":%lu", (unsigned long)id);
        if (g.note_len && n + g.note_len + 10 < sizeof(json)) {
            n += snprintf(json + n, sizeof(json) - n, ",\"data\":%s", g.note);
        } else if (g.note_len) {
            n += snprintf(json + n, sizeof(json) - n, ",\"data_bytes\":%u", (unsigned)g.note_len);
        }
        strlcpy(json + n, "}", sizeof(json) - n);
        g.note_busy = false;
        emit("incident", json, 0, false);
        post_async(r.slot, r.seq, ESP_OK, id);
    }

    void priv::incident_data(const req &r)
    {
        if (g.cfg.incidents && r.addr && g.note_len) {
            g.cfg.incidents->add(r.addr, incident_part::data, g.note, g.note_len);
        }
        g.note_busy = false;
    }

    static JSValue js_watches(JSContext *ctx, JSValueConst, int, JSValueConst *)
    {
        return list_watches(ctx);
    }

    static JSValue js_permissions(JSContext *ctx, JSValueConst, int, JSValueConst *)
    {
        JSValue o = JS_NewObject(ctx);
        JS_SetPropertyStr(ctx, o, "write", JS_NewBool(ctx, g.cfg.perms.write));
        JS_SetPropertyStr(ctx, o, "flash", JS_NewBool(ctx, g.cfg.perms.flash));
        JS_SetPropertyStr(ctx, o, "reset", JS_NewBool(ctx, g.cfg.perms.reset));
        return o;
    }

    static const JSCFunctionListEntry agent_funcs[] = {
        JS_CFUNC_DEF("emit", 2, js_emit),
        JS_CFUNC_DEF("incident", 2, js_incident),
        JS_CFUNC_DEF("watches", 0, js_watches),
        JS_CFUNC_DEF("permissions", 0, js_permissions),
    };

    static int agent_init(JSContext *ctx, JSModuleDef *m)
    {
        return JS_SetModuleExportList(ctx, m, agent_funcs, sizeof(agent_funcs) / sizeof(agent_funcs[0]));
    }

    bool priv::init_agent_module(JSContext *ctx)
    {
        JSModuleDef *m = JS_NewCModule(ctx, "si:agent", agent_init);
        return m && JS_AddModuleExportList(ctx, m, agent_funcs, sizeof(agent_funcs) / sizeof(agent_funcs[0])) == 0;
    }

    bool priv::add_agent_global(JSContext *ctx, JSValueConst global)
    {
        JSValue o = JS_NewObject(ctx);
        if (JS_IsException(o) || JS_SetPropertyFunctionList(ctx, o, agent_funcs, sizeof(agent_funcs) / sizeof(agent_funcs[0])) < 0) {
            JS_FreeValue(ctx, o);
            return false;
        }
        return JS_SetPropertyStr(ctx, global, "agent", o) >= 0;
    }

    static void clear_pending()
    {
        memset(g.pend, 0, sizeof(g.pend));
    }

    static bool reap_runtime(uint32_t wait_ms)
    {
        if (!g.rt_started) {
            return true;
        }
        if (xSemaphoreTake(g.ended, wait_ms ? ms_ticks(wait_ms) : 0) != pdTRUE) {
            return false;
        }
        g.rt_started = false;
        clear_pending();
        return true;
    }

    static esp_err_t stop_runtime()
    {
        if (!g.rt_started) {
            return ESP_OK;
        }
        g.stop_wanted = true;
        stop_loop();
        if (!reap_runtime(GRACE_MS)) {
            g.dead = true;
            ESP_LOGE(TAG, "JS task didn't stop the session runtime, scripts off until reboot");
            return ESP_ERR_TIMEOUT;
        }
        return ESP_OK;
    }

    static void start_runtime(uint8_t kind, uint32_t token, uint8_t *blob, size_t len)
    {
        xQueueReset(g.req_q);
        xQueueReset(g.res_q);
        xSemaphoreTake(g.ended, 0);
        clear_pending();
        g.note_busy = false;
        g.stop_wanted = false;
        g.rt_ending = false;
        g.module_failed = false;
        g.first_kind = kind;
        g.first_token = token;
        g.first_blob = blob;
        g.first_len = len;
        g.rt_started = true;
        uint8_t cmd = CMD_SESSION;
        xQueueSend(g.cmd_q, &cmd, portMAX_DELAY); // the JS task is idle: the last runtime was reaped
    }

    static esp_err_t add_pending(const req &r)
    {
        for (auto &p : g.pend) {
            if (p.used) {
                continue;
            }
            int64_t now = now_us();
            uint32_t ms = r.op == OP_DELAY ? r.a : std::min(r.timeout_ms, MAX_WAIT_MS);
            p = {true, r, now + ms * 1000LL, now};
            return ESP_OK;
        }
        return ESP_ERR_NO_MEM;
    }

    static void step_pending(js_target &t)
    {
        for (auto &p : g.pend) {
            if (!p.used) {
                continue;
            }
            int64_t now = now_us();
            if (now < p.next_us && now < p.until_us) {
                continue;
            }
            bool done = false;
            esp_err_t ret = ESP_OK;
            uint32_t val = 0;
            switch (p.r.op) {
            case OP_DELAY:
                done = now >= p.until_us;
                break;
            case OP_POLL32:
                ret = t.read32(p.r.addr, &val);
                done = ret != ESP_OK || (val & p.r.a) == p.r.b;
                if (!done && now >= p.until_us) {
                    done = true;
                    ret = ESP_ERR_TIMEOUT;
                }
                p.next_us = now + std::max<uint32_t>(p.r.interval_ms, 1) * 1000LL;
                break;
            case OP_WAIT_HALT: {
                core_state st = {};
                ret = t.poll_core(&st);
                g.eng.reset_latch |= st.reset_seen;
                done = ret != ESP_OK || st.halted;
                if (!done && now >= p.until_us) {
                    done = true;
                    ret = ESP_ERR_TIMEOUT;
                }
                p.next_us = now + 1000;
                break;
            }
            default:
                done = true;
                ret = ESP_ERR_NOT_SUPPORTED;
            }
            if (done) {
                p.used = false;
                post_async(p.r.slot, p.r.seq, ret, val);
            }
        }
    }

    static void handle(const req &r, js_target &t)
    {
        switch (r.op) {
        case OP_DONE:
            g.done_token = r.seq;
            return;
        case OP_WATCH_ACTION:
            watch_verdict(r, t);
            return;
        case OP_INCIDENT:
            incident_from_script(r);
            return;
        case OP_INCIDENT_DATA:
            incident_data(r);
            return;
        case OP_POLL32:
        case OP_WAIT_HALT:
        case OP_DELAY:
            if (r.slot < MAX_ASYNC && add_pending(r) == ESP_OK) {
                return; // stepped between other work, so watches keep running
            }
            break;
        default:
            break;
        }
        watch_before_op(r.op, r.addr, t);
        esp_err_t ret = execute(r, &t, nullptr);
        watch_after_op(r.op, r.addr, ret);
    }

    bool priv::result_pending()
    {
        const uint32_t want = g.want_token.load() & ~CLAIMED;
        return want != 0 && want != g.done_token;
    }

    void js_host::session_service(js_target &target, uint32_t wait_ms)
    {
        if (!g.session) {
            vTaskDelay(ms_ticks(wait_ms));
            return;
        }
        req r;
        TickType_t wait = wait_ms ? ms_ticks(wait_ms) : 0;
        for (int i = 0; i < SERVE_BATCH && xQueueReceive(g.req_q, &r, i == 0 ? wait : 0) == pdTRUE; i++) {
            handle(r, target);
        }
        watch_poll(target);
        step_pending(target);
        if (g.rt_started && !g.alive && reap_runtime(0)) {
            ESP_LOGI(TAG, "session runtime reaped");
        }
    }

    static uint32_t next_token()
    {
        g.token = (g.token + 1) & ~CLAIMED;
        if (g.token == 0) {
            g.token = 1;
        }
        return g.token;
    }

    static esp_err_t wait_result(uint32_t token, uint32_t timeout_ms, js_target &t)
    {
        uint32_t ms = timeout_ms ? std::min(timeout_ms, MAX_WAIT_MS) : DEFAULT_WAIT_MS;
        int64_t end = now_us() + ms * 1000LL;
        while (g.done_token != token && now_us() < end) {
            js_host::instance()->session_service(t, 5);
        }
        if (g.want_token.exchange(0) == (token | CLAIMED)) { // its result is being written: take it
            end = now_us() + CLAIMED_WAIT_MS * 1000LL;
            while (g.done_token != token && now_us() < end) {
                js_host::instance()->session_service(t, 1);
            }
        }
        g.out_pending = g.done_token != token;
        return ESP_OK;
    }

    static esp_err_t check_blob(const uint8_t *blob, size_t len)
    {
        if (!g.session) {
            return ESP_ERR_INVALID_STATE;
        }
        if (!g.ready || g.dead) {
            return ESP_ERR_NOT_SUPPORTED;
        }
        if (len < QJS_ESP_BC_HDR_LEN || len > CONFIG_SI_JS_MAX_SCRIPT_KB * 1024) {
            return ESP_ERR_INVALID_SIZE;
        }
        return ESP_OK;
    }

    static uint8_t *copy_blob(const uint8_t *blob, size_t len, uint8_t sha[32], bool *quarantined)
    {
        size_t hash_len = 0;
        if (psa_hash_compute(PSA_ALG_SHA_256, blob, len, sha, 32, &hash_len) != PSA_SUCCESS) {
            memset(sha, 0, 32);
        }
        *quarantined = is_quarantined(sha);
        if (*quarantined) {
            return nullptr;
        }
        auto *p = static_cast<uint8_t *>(heap_caps_malloc(len, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
        if (p) {
            memcpy(p, blob, len);
        }
        return p;
    }

    static void owner_result(script_outcome o, const char *error) // the JS task may still write the res_ buffers
    {
        g.want_token = 0;
        g.out_pending = false;
        g.owner_outcome = o;
        g.owner_note = error;
    }

    // *token 0: answered without a runtime (last_script_result() has why).
    esp_err_t js_host::session_start(const uint8_t *blob, size_t len, uint32_t *token)
    {
        *token = 0;
        esp_err_t ret = check_blob(blob, len);
        if (ret != ESP_OK) {
            return ret;
        }
        uint8_t sha[32];
        bool quarantined = false;
        uint8_t *copy = copy_blob(blob, len, sha, &quarantined);
        ret = stop_runtime();
        if (ret != ESP_OK) {
            heap_caps_free(copy);
            return ESP_ERR_NOT_SUPPORTED;
        }
        if (quarantined || !copy) {
            owner_result(script_outcome::error, quarantined ? "this script crashed the device, reboot to retry" : "no room for the script");
            return ESP_OK;
        }
        *token = next_token();
        g.want_token = *token;
        g.owner_note = nullptr;
        g.out_pending = false;
        set_crash_mark(sha);
        start_runtime(FIRST_MODULE, *token, copy, len);
        return ESP_OK;
    }

    bool js_host::session_poll_result(uint32_t token, bool give_up, js_target &target)
    {
        if (token == 0) {
            return true;
        }
        if (g.done_token != token && !give_up) {
            return false;
        }
        if (g.want_token.exchange(0) == (token | CLAIMED)) { // its result is being written: take it
            int64_t end = now_us() + CLAIMED_WAIT_MS * 1000LL;
            while (g.done_token != token && now_us() < end) {
                session_service(target, 1);
            }
        }
        g.out_pending = g.done_token != token;
        return true;
    }

    esp_err_t js_host::session_run(const uint8_t *blob, size_t len, uint32_t timeout_ms, js_target &target)
    {
        uint32_t token = 0;
        esp_err_t ret = session_start(blob, len, &token);
        if (ret != ESP_OK || token == 0) {
            return ret;
        }
        return wait_result(token, timeout_ms, target);
    }

    esp_err_t js_host::session_eval(const uint8_t *blob, size_t len, uint32_t timeout_ms, js_target &target)
    {
        esp_err_t ret = check_blob(blob, len);
        if (ret != ESP_OK) {
            return ret;
        }
        uint8_t sha[32];
        bool quarantined = false;
        uint8_t *copy = copy_blob(blob, len, sha, &quarantined);
        if (quarantined || !copy) {
            heap_caps_free(copy);
            owner_result(script_outcome::error, quarantined ? "this script crashed the device, reboot to retry" : "no room for the script");
            return ESP_OK;
        }
        uint32_t token = next_token();
        g.want_token = token;
        g.owner_note = nullptr;
        set_crash_mark(sha);
        auto *p = static_cast<eval_post *>(heap_caps_malloc(sizeof(eval_post), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
        for (int attempt = 0; p && attempt < 2; attempt++) {
            if (g.rt_started && g.rt_ending && stop_runtime() != ESP_OK) { // ended by itself: let it finish
                break;
            }
            if (!g.rt_started) {
                heap_caps_free(p);
                start_runtime(FIRST_EVAL, token, copy, len);
                return wait_result(token, timeout_ms, target);
            }
            int64_t until = now_us() + START_MS * 1000LL;
            while (!g.alive && !g.rt_ending && now_us() < until) { // still starting
                session_service(target, 5);
            }
            *p = {token, copy, len};
            xSemaphoreTake(g.loop_lock, portMAX_DELAY);
            bool posted = g.loop && g.alive && qjs_esp_loop_post(g.loop, eval_work, p, pdMS_TO_TICKS(100)) == ESP_OK;
            xSemaphoreGive(g.loop_lock);
            if (posted) {
                return wait_result(token, timeout_ms, target);
            }
        }
        heap_caps_free(p);
        heap_caps_free(copy);
        owner_result(script_outcome::error, g.dead ? "the script runtime is stuck, scripts are off until reboot"
                                                   : "the script runtime isn't taking work, try again");
        return ESP_OK;
    }

    void js_host::session_stop(js_target &target)
    {
        if (g.session) {
            stop_runtime();
            watch_poll(target); // drops the vector catch of the watches that went
        }
    }

    esp_err_t js_host::begin_session(const session_config &cfg)
    {
        if (!g.ready || g.dead) {
            return ESP_ERR_NOT_SUPPORTED;
        }
        if (!claim(USER_SESSION)) {
            return ESP_ERR_INVALID_STATE; // a job run, or another session
        }
        g.cfg = cfg;
        g.hook_end_us = INT64_MAX;
        g.done_token = 0;
        g.want_token = 0;
        g.out_pending = false;
        g.rt_started = false;
        clear_pending();
        xQueueReset(g.req_q);
        xQueueReset(g.res_q);
        xSemaphoreTake(g.ended, 0);
        owner_result(script_outcome::ok, "");
        watch_reset_engine();
        xSemaphoreTake(g.ev_lock, portMAX_DELAY);
        g.ev_dropped = 0;
        g.ev_tokens = EV_BURST;
        g.ev_refill_us = now_us();
        xSemaphoreGive(g.ev_lock);
        g.session = true;
        return ESP_OK;
    }

    void js_host::end_session(js_target &target)
    {
        if (!g.session) {
            return;
        }
        stop_runtime();
        watch_disarm(target);
        g.session = false;
        g.want_token = 0;
        set_crash_mark(nullptr);
        clear_pending();
        g.user = USER_NONE;
    }

    bool js_host::in_session() const
    {
        return g.session;
    }

    bool js_host::module_failed() const
    {
        return g.module_failed;
    }

    bool js_host::session_running() const
    {
        return g.session && g.rt_started;
    }

    bool js_host::target_lost() const
    {
        return g.session && g.eng.lost;
    }

    bool js_host::refuses(const uint8_t sha256[32]) const
    {
        return is_quarantined(sha256);
    }

    script_result js_host::last_script_result() const
    {
        if (g.out_pending) {
            return {script_outcome::pending, false, "", "", ""};
        }
        if (g.owner_note) {
            return {g.owner_outcome, false, "", g.owner_note, ""};
        }
        return {g.res_outcome, g.res_truncated, g.res_value, g.res_error, g.res_stack};
    }

    size_t js_host::read_events(uint32_t after_seq, event_out *out, size_t max, uint32_t *newest_seq, uint32_t *dropped)
    {
        if (!g.events) {
            return 0;
        }
        xSemaphoreTake(g.ev_lock, portMAX_DELAY);
        uint32_t newest = g.ev_seq;
        uint32_t oldest = newest >= EVENTS ? newest - EVENTS + 1 : 1;
        size_t n = 0;
        for (uint32_t seq = std::max(after_seq + 1, oldest); seq <= newest && seq > after_seq && n < max; seq++) {
            const ev_slot &e = g.events[seq % EVENTS];
            if (e.seq != seq) {
                continue;
            }
            event_out &o = out[n++];
            o.seq = e.seq;
            o.uptime_us = e.uptime_us;
            o.capture_id = e.capture_id;
            strlcpy(o.kind, e.kind, sizeof(o.kind));
            strlcpy(o.json, e.json, sizeof(o.json));
        }
        if (newest_seq) {
            *newest_seq = newest;
        }
        if (dropped) {
            *dropped = g.ev_dropped;
        }
        xSemaphoreGive(g.ev_lock);
        return n;
    }

    esp_err_t js_host::read_capture(uint32_t id, uint32_t offset, uint8_t *buf, size_t cap, size_t *len, uint32_t *total)
    {
        for (const auto &c : g.caps) {
            if (id == 0 || c.id != id) {
                continue;
            }
            if (offset > c.len) {
                return ESP_ERR_INVALID_ARG;
            }
            size_t n = std::min<size_t>(cap, c.len - offset);
            memcpy(buf, c.data + offset, n);
            *len = n;
            *total = c.len;
            return ESP_OK;
        }
        return ESP_ERR_NOT_FOUND;
    }
} // namespace qjs_binder

#else // stubs

namespace qjs_binder
{
    esp_err_t js_host::begin_session(const session_config &) { return ESP_ERR_NOT_SUPPORTED; }
    esp_err_t js_host::session_run(const uint8_t *, size_t, uint32_t, js_target &) { return ESP_ERR_NOT_SUPPORTED; }
    esp_err_t js_host::session_start(const uint8_t *, size_t, uint32_t *token)
    {
        *token = 0;
        return ESP_ERR_NOT_SUPPORTED;
    }
    bool js_host::session_poll_result(uint32_t, bool, js_target &) { return true; }
    esp_err_t js_host::session_eval(const uint8_t *, size_t, uint32_t, js_target &) { return ESP_ERR_NOT_SUPPORTED; }
    void js_host::session_stop(js_target &) {}
    void js_host::session_service(js_target &, uint32_t wait_ms) { vTaskDelay(pdMS_TO_TICKS(wait_ms) ? pdMS_TO_TICKS(wait_ms) : 1); }
    void js_host::end_session(js_target &) {}
    bool js_host::in_session() const { return false; }
    bool js_host::session_running() const { return false; }
    bool js_host::module_failed() const { return false; }
    bool js_host::target_lost() const { return false; }
    bool js_host::refuses(const uint8_t *) const { return false; }
    script_result js_host::last_script_result() const { return {script_outcome::error, false, "", "no scripts", ""}; }
    size_t js_host::read_events(uint32_t, event_out *, size_t, uint32_t *newest, uint32_t *dropped)
    {
        if (newest) {
            *newest = 0;
        }
        if (dropped) {
            *dropped = 0;
        }
        return 0;
    }
    esp_err_t js_host::read_capture(uint32_t, uint32_t, uint8_t *, size_t, size_t *, uint32_t *) { return ESP_ERR_NOT_FOUND; }
} // namespace qjs_binder

#endif
