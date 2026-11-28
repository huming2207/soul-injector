#include <cstdio>
#include <algorithm>
#include <cstring>

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include <esp_attr.h>
#include <esp_heap_caps.h>
#include <esp_log.h>
#include <esp_rom_sys.h>
#include <esp_system.h>
#include <esp_timer.h>
#include <psa/crypto.h>

#include "host_priv.hpp"
#include "quickjs-libc.h"

#if CONFIG_SI_JS_ENABLE

namespace qjs_binder
{
    using namespace priv;

    state priv::g = {};

    static const char *TAG = "js_host";

#if CONFIG_SI_JS_NETWORK
    static constexpr bool HAVE_NETWORK = true;
#else
    static constexpr bool HAVE_NETWORK = false;
#endif
    static constexpr uint32_t LOAD_TIMEOUT_MS = 5000;
    static constexpr uint32_t GRACE_MS = 2000;      // past a deadline
    static constexpr uint32_t CANCEL_POLL_MS = 50;
    static constexpr uint32_t CALL_MAX_MS = 60000; // target.call(): a session has no hook deadline to bound it
    static constexpr const char *HOOK_NAMES[HOOKS] = {"preProgram", "postProgram", "test", "onFail"};

    static __NOINIT_ATTR struct {
        uint32_t magic;
        uint8_t sha256[32];
    } crash_mark;
    static constexpr uint32_t CRASH_MAGIC = 0x4a53524e;
    static uint8_t quarantined[32];
    static bool have_quarantine;

    bool priv::is_quarantined(const uint8_t sha256[32])
    {
        return have_quarantine && memcmp(quarantined, sha256, sizeof(quarantined)) == 0;
    }

    void priv::set_crash_mark(const uint8_t *sha256)
    {
        if (sha256) {
            memcpy(crash_mark.sha256, sha256, sizeof(crash_mark.sha256));
            crash_mark.magic = CRASH_MAGIC;
        } else {
            crash_mark.magic = 0;
        }
    }

    bool priv::claim(uint8_t who)
    {
        uint8_t none = USER_NONE;
        return g.user.compare_exchange_strong(none, who);
    }

    int64_t priv::now_us()
    {
        return esp_timer_get_time();
    }

    static constexpr char ENDED_BETWEEN[] = "script ended between hooks: ";

    static void set_err(const char *fmt, const char *arg = "")
    {
        snprintf(g.err, sizeof(g.err), fmt, arg);
        g.js_err_set = false;
    }

    // A string property of `e` into `out`, empty when it has none.
    static void prop_text(JSContext *ctx, JSValueConst e, const char *name, char *out, size_t len)
    {
        out[0] = '\0';
        JSValue v = JS_GetPropertyStr(ctx, e, name);
        if (JS_IsString(v)) {
            const char *t = JS_ToCString(ctx, v);
            if (t) {
                strlcpy(out, t, len);
                JS_FreeCString(ctx, t);
            }
        }
        JS_FreeValue(ctx, v);
    }

    // One line per runtime, so a long run of jobs or a trap that restarts for
    // days shows whether the device heap outside the arena drifts, and how
    // close scripts come to the arena's size.
    void priv::log_runtime_end(const char *what)
    {
        ESP_LOGI(TAG, "%s ended: arena peak %u of %u KB; free internal %u (min %u), PSRAM %u (min %u)", what,
                 (unsigned)(qjs_esp_arena_peak(g.arena) / 1024), (unsigned)CONFIG_SI_JS_ARENA_KB,
                 (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
                 (unsigned)heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL),
                 (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM),
                 (unsigned)heap_caps_get_minimum_free_size(MALLOC_CAP_SPIRAM));
    }

    bool priv::ends_runtime(JSValueConst e)
    {
        if (!JS_IsUncatchableError(e)) {
            return false;
        }
        if (g.loop) {
            qjs_esp_loop_fail(g.loop, e); // no-op when it's the loop's own interrupt
        }
        return true;
    }

    void priv::record_error(JSContext *ctx, JSValueConst e)
    {
        const char *msg = JS_ToCString(ctx, e);
        JSValue stack = JS_IsError(e) ? JS_GetPropertyStr(ctx, e, "stack") : JS_UNDEFINED;
        const char *st = JS_IsString(stack) ? JS_ToCString(ctx, stack) : nullptr;
        ESP_LOGE(TAG, "script: %s\n%s", msg ? msg : "(error)", st ? st : "");
        snprintf(g.err, sizeof(g.err), "%s", msg ? msg : "script error");
        g.js_err_set = JS_IsError(e);
        if (g.js_err_set) {
            prop_text(ctx, e, "name", g.js_err_name, sizeof(g.js_err_name));
            prop_text(ctx, e, "message", g.js_err_msg, sizeof(g.js_err_msg));
            prop_text(ctx, e, "code", g.js_err_code, sizeof(g.js_err_code));
        }
        JS_FreeCString(ctx, st);
        JS_FreeCString(ctx, msg);
        JS_FreeValue(ctx, stack);
    }

    static void send_done(esp_err_t ret)
    {
        req r = {};
        r.op = OP_DONE;
        r.ret = ret;
        if (xQueueSend(g.req_q, &r, pdMS_TO_TICKS(100)) != pdTRUE) {
            ESP_LOGE(TAG, "owner queue full, result lost"); // owner's deadline catches it
        }
    }

    static void finish(JSContext *ctx, esp_err_t ret)
    {
        if (!g.hook_active && !g.loading) {
            return;
        }
        if (g.loading && ret == ESP_OK) {
            JSValue ns = JS_GetModuleNamespace(ctx, g.mod);
            for (int i = 0; i < HOOKS; i++) {
                JSValue fn = JS_GetPropertyStr(ctx, ns, HOOK_NAMES[i]);
                g.hooks[i] = JS_IsFunction(ctx, fn);
                JS_FreeValue(ctx, fn);
            }
            JS_FreeValue(ctx, ns);
        }
        g.loading = false;
        g.hook_active = false;
        async_reset(ctx);
        qjs_esp_loop_set_deadline(g.loop, 0);
        send_done(ret);
    }

    static JSValue on_settled(JSContext *ctx, JSValueConst, int argc, JSValueConst *argv, int magic, JSValueConst *)
    {
        if (magic && argc && ends_runtime(argv[0])) {
            return JS_UNDEFINED; // see settle()
        }
        if (magic) {
            record_error(ctx, argc ? argv[0] : JS_UNDEFINED);
        }
        finish(ctx, magic ? ESP_FAIL : ESP_OK);
        return JS_UNDEFINED;
    }

    static void settle(JSContext *ctx, JSValue r)
    {
        if (JS_IsException(r)) {
            JSValue e = JS_GetException(ctx);
            bool stopped = ends_runtime(e); // run_script() answers when the loop returns
            if (!stopped) {
                record_error(ctx, e);
            }
            JS_FreeValue(ctx, e);
            if (!stopped) {
                finish(ctx, ESP_FAIL);
            }
            return;
        }
        if (!JS_IsPromise(r)) {
            JS_FreeValue(ctx, r);
            finish(ctx, ESP_OK);
            return;
        }
        JSValue ok = JS_NewCFunctionData(ctx, on_settled, 1, 0, 0, nullptr);
        JSValue bad = JS_NewCFunctionData(ctx, on_settled, 1, 1, 0, nullptr);
        // Handlers that failed to allocate would be taken as none: the hook
        // would never be seen to settle
        JSValue p = JS_IsException(ok) || JS_IsException(bad) ? JS_EXCEPTION : JS_PromiseThen(ctx, r, ok, bad);
        if (JS_IsException(p)) {
            JS_FreeValue(ctx, JS_GetException(ctx));
            set_err("out of memory");
            finish(ctx, ESP_ERR_NO_MEM);
        }
        JS_FreeValue(ctx, p);
        JS_FreeValue(ctx, ok);
        JS_FreeValue(ctx, bad);
        JS_FreeValue(ctx, r);
    }

    static JSValue js_console(JSContext *ctx, JSValueConst, int argc, JSValueConst *argv, int level)
    {
        char line[256];
        size_t n = 0;
        line[0] = '\0';
        for (int i = 0; i < argc && n < sizeof(line) - 1; i++) {
            const char *s = JS_ToCString(ctx, argv[i]);
            n += snprintf(line + n, sizeof(line) - n, "%s%s", i ? " " : "", s ? s : "?");
            JS_FreeCString(ctx, s);
        }
        if (g.session) {
            session_console(level, line);
        }
        if (level == 2) {
            ESP_LOGE("js", "%s", line);
        } else if (level == 1) {
            ESP_LOGW("js", "%s", line);
        } else {
            ESP_LOGI("js", "%s", line);
        }
        return JS_UNDEFINED;
    }

    static const JSCFunctionListEntry console_funcs[] = {
        JS_CFUNC_MAGIC_DEF("log", 1, js_console, 0),
        JS_CFUNC_MAGIC_DEF("info", 1, js_console, 0),
        JS_CFUNC_MAGIC_DEF("warn", 1, js_console, 1),
        JS_CFUNC_MAGIC_DEF("error", 1, js_console, 2),
    };

    JSModuleDef *priv::refuse_module(JSContext *ctx, const char *name, void *)
    {
        JS_ThrowReferenceError(ctx, "no module '%s' here (bundle local files into the script)", name);
        return nullptr;
    }

    static JSContext *new_context(JSRuntime *rt, bool session)
    {
        JSContext *ctx = JS_NewContextRaw(rt);
        if (!ctx) {
            return nullptr;
        }
        if (JS_AddIntrinsicBaseObjects(ctx) || JS_AddIntrinsicDate(ctx) || JS_AddIntrinsicRegExp(ctx) ||
            JS_AddIntrinsicJSON(ctx) || JS_AddIntrinsicMapSet(ctx) || JS_AddIntrinsicTypedArrays(ctx) ||
            JS_AddIntrinsicPromise(ctx) || JS_AddIntrinsicAToB(ctx) || JS_AddPerformance(ctx) ||
            !init_target_module(ctx) || !init_misc_modules(ctx, session)) {
            JS_FreeContext(ctx);
            return nullptr;
        }
        JSValue global = JS_GetGlobalObject(ctx);
        JSValue console = JS_NewObject(ctx);
        JS_SetPropertyFunctionList(ctx, console, console_funcs, sizeof(console_funcs) / sizeof(console_funcs[0]));
        JS_SetPropertyStr(ctx, global, "console", console);
        bool ok = !session || (add_target_globals(ctx, global) && add_misc_globals(ctx, global));
        JS_FreeValue(ctx, global);
        if (!ok) {
            JS_FreeContext(ctx);
            return nullptr;
        }
#if CONFIG_SI_JS_NETWORK
        if (!session && g.network && !qjs_esp_init_module_ws(ctx, "esp:ws")) {
            JS_FreeContext(ctx);
            return nullptr;
        }
#endif
        return ctx;
    }

    static JSValue job_info(JSContext *ctx)
    {
        JSValue job = JS_NewObject(ctx);
        JS_SetPropertyStr(ctx, job, "name", JS_NewString(ctx, g.job_name));
        JS_SetPropertyStr(ctx, job, "variant", JS_NewString(ctx, g.variant));
        JS_SetPropertyStr(ctx, job, "family", JS_NewString(ctx, g.family));
        JSValue c = JS_NewObject(ctx);
        JS_SetPropertyStr(ctx, c, "hook", JS_NewString(ctx, HOOK_NAMES[(int)g.cur_hook]));
        JS_SetPropertyStr(ctx, c, "job", job);
        return c;
    }

    static void call_hook(JSContext *ctx, void *)
    {
        if (loop_stopping()) {
            if (g.alive) { // still in the loop: run_script() answers for it as for a hook in flight
                g.hook_active = true;
                return;
            }
            // qjs_esp_loop_free(): posted after the loop stopped, before
            // run_script() cleared g.alive. Answer, or run_hook() waits out its timeout.
            if (strncmp(g.err, ENDED_BETWEEN, strlen(ENDED_BETWEEN)) != 0) {
                set_err("script isn't running");
            }
            send_done(ESP_ERR_INVALID_STATE);
            return;
        }
        g.hook_active = true;
        qjs_esp_loop_set_deadline(g.loop, now_us() + g.cur_timeout_ms * 1000LL);
        qjs_esp_loop_set_limits(g.loop, &g.lim); // fresh slice for the hook

        JSValue ns = JS_GetModuleNamespace(ctx, g.mod);
        JSValue fn = JS_GetPropertyStr(ctx, ns, HOOK_NAMES[(int)g.cur_hook]);
        JSValue args[3] = {job_info(ctx), JS_UNDEFINED, JS_UNDEFINED};
        int argc = 1;
        if (g.cur_hook == hook::on_fail) {
            args[1] = JS_NewString(ctx, g.fail_stage);
            args[2] = JS_NewError(ctx);
            if (g.fail_js) { // the hook's own error, as it threw it (less the stack)
                if (g.js_err_name[0]) {
                    JS_SetPropertyStr(ctx, args[2], "name", JS_NewString(ctx, g.js_err_name));
                }
                JS_SetPropertyStr(ctx, args[2], "message", JS_NewString(ctx, g.js_err_msg));
                if (g.js_err_code[0]) {
                    JS_SetPropertyStr(ctx, args[2], "code", JS_NewString(ctx, g.js_err_code));
                }
            } else {
                JS_SetPropertyStr(ctx, args[2], "message", JS_NewString(ctx, g.fail_reason));
            }
            argc = 3;
        }
        JSValue r = JS_IsFunction(ctx, fn) ? JS_Call(ctx, fn, JS_UNDEFINED, argc, args)
                                           : JS_ThrowTypeError(ctx, "no %s hook", HOOK_NAMES[(int)g.cur_hook]);
        for (auto &a : args) {
            JS_FreeValue(ctx, a);
        }
        JS_FreeValue(ctx, fn);
        JS_FreeValue(ctx, ns);
        settle(ctx, r);
    }

    const char *priv::stop_text(qjs_esp_stop_t why)
    {
        switch (why) {
        case QJS_ESP_STOP_DEADLINE:
            return "script ran out of time";
        case QJS_ESP_STOP_SLICE:
            return "script ran too long without yielding";
        case QJS_ESP_STOP_REQUESTED:
            return "script stopped";
        case QJS_ESP_STOP_ERROR:
            // Uncatchable, so the script never saw it: usually "InternalError: out of memory".
            return g.loop && qjs_esp_loop_error(g.loop)[0] ? qjs_esp_loop_error(g.loop) : "script failed";
        default:
            return "script ended";
        }
    }

    static void run_script()
    {
        JSRuntime *rt = qjs_esp_new_runtime_in(g.arena);
        if (!rt) {
            qjs_esp_arena_reset(g.arena); // leftovers are garbage
            rt = qjs_esp_new_runtime_in(g.arena);
        }
        JSContext *ctx = rt ? (js_std_init_handlers(rt), new_context(rt, false)) : nullptr;
        qjs_esp_loop_t *loop = ctx ? qjs_esp_loop_new(ctx) : nullptr;
        if (!loop) {
            set_err("no room for the script runtime");
            send_done(ESP_ERR_NO_MEM);
            qjs_esp_arena_reset(g.arena);
            return;
        }
        JS_SetModuleLoaderFunc(rt, nullptr, refuse_module, nullptr);

        g.lim = {CONFIG_SI_JS_SLICE_MS, CONFIG_SI_JS_YIELD_MS};
        qjs_esp_loop_set_limits(loop, &g.lim);
        qjs_esp_loop_keep_alive(loop, true); // hooks come as posted work
        qjs_esp_loop_set_deadline(loop, now_us() + LOAD_TIMEOUT_MS * 1000LL);
        xSemaphoreTake(g.loop_lock, portMAX_DELAY);
        g.loop = loop;
        g.alive = true;
        xSemaphoreGive(g.loop_lock);

        g.results = JS_NewObject(ctx);
        g.on_progress = JS_UNDEFINED;
        g.results_json[0] = '\0';
        g.xfer_busy = false;
        g.loading = true;
        memset(g.hooks, 0, sizeof(g.hooks));
        settle(ctx, qjs_esp_bc_eval(ctx, g.script, g.script_len, &g.mod));

        qjs_esp_stop_t why = qjs_esp_loop_run(loop);
        xSemaphoreTake(g.loop_lock, portMAX_DELAY); // run_hook() checks it under the lock
        g.alive = false; // before send_done, so no hook is posted to a stopped loop
        xSemaphoreGive(g.loop_lock);
        if (g.loading || g.hook_active) {
            set_err("%s", stop_text(why));
            ESP_LOGE(TAG, "%s", g.err);
            g.loading = g.hook_active = false;
            send_done(why == QJS_ESP_STOP_REQUESTED ? ESP_ERR_INVALID_STATE
                      : why == QJS_ESP_STOP_ERROR   ? ESP_FAIL
                                                    : ESP_ERR_TIMEOUT);
        } else if (why != QJS_ESP_STOP_REQUESTED) { // not end_run(): it died in a callback, e.g. onProgress
            snprintf(g.err, sizeof(g.err), "%s%s", ENDED_BETWEEN, stop_text(why));
            g.js_err_set = false;
            ESP_LOGE(TAG, "%s", g.err);
        }

        xSemaphoreTake(g.loop_lock, portMAX_DELAY);
        qjs_esp_loop_free(loop); // drains posted work
        g.loop = nullptr;
        xSemaphoreGive(g.loop_lock);
        memset(g.slots, 0, sizeof(g.slots));
        g.mod = nullptr;
        g.results = g.on_progress = JS_UNDEFINED;
        log_runtime_end("job script runtime");
        qjs_esp_arena_reset(g.arena);
    }

    JSContext *priv::new_session_context(JSRuntime *rt)
    {
        return new_context(rt, true);
    }

    static void js_task(void *)
    {
        for (;;) {
            uint8_t cmd;
            xQueueReceive(g.cmd_q, &cmd, portMAX_DELAY);
            if (cmd == CMD_SESSION) {
                run_session_runtime();
            } else {
                run_script();
            }
            xSemaphoreGive(g.ended);
        }
    }

    TickType_t priv::ms_ticks(uint32_t ms)
    {
        TickType_t t = pdMS_TO_TICKS(ms);
        return t ? t : 1;
    }

    bool priv::loop_stopping()
    {
        return g.loop && qjs_esp_loop_stopping(g.loop) != QJS_ESP_STOP_IDLE;
    }

    void priv::stop_loop()
    {
        xSemaphoreTake(g.loop_lock, portMAX_DELAY);
        if (g.loop) {
            qjs_esp_loop_stop(g.loop);
        }
        xSemaphoreGive(g.loop_lock);
    }

    static esp_err_t expired(int64_t until_us, cancel_fn cancelled)
    {
        if (cancelled && cancelled()) {
            return ESP_ERR_NOT_FINISHED; // STOPPED
        }
        return now_us() >= until_us || now_us() >= g.hook_end_us ? ESP_ERR_TIMEOUT : ESP_OK;
    }

    static esp_err_t run_op(const req &r, js_target &t, cancel_fn cancelled, uint32_t *val)
    {
        switch (r.op) {
        case OP_READ32:
            return t.read32(r.addr, val);
        case OP_WRITE32:
            return t.write32(r.addr, r.a);
        case OP_RMW32: {
            esp_err_t ret = t.read32(r.addr, val);
            if (ret != ESP_OK) {
                return ret;
            }
            *val = (*val & r.a) | r.b;
            return t.write32(r.addr, *val);
        }
        case OP_HALT:
            return t.halt();
        case OP_READ_MEM:
            return t.read_mem(r.addr, g.xfer, r.a);
        case OP_WRITE_MEM:
            return t.write_mem(r.addr, g.xfer, r.a);
        case OP_WAIT_HALT:
            return t.wait_halt();
        case OP_RESET:
            return t.reset();
        case OP_REINIT:
            return t.reinit();
        case OP_POLL32: {
            int64_t until = now_us() + r.timeout_ms * 1000LL;
            for (;;) {
                esp_err_t ret = t.read32(r.addr, val);
                if (ret != ESP_OK || (*val & r.a) == r.b) {
                    return ret;
                }
                // The interval in slices: a long one mustn't outlast the
                // timeout or a cancel (expired() checks both)
                const int64_t next = now_us() + r.interval_ms * 1000LL;
                esp_err_t over;
                do {
                    over = expired(until, cancelled);
                    if (over != ESP_OK) {
                        return over;
                    }
                    const int64_t left_ms = (std::min(next, std::min(until, g.hook_end_us)) - now_us()) / 1000;
                    vTaskDelay(ms_ticks(left_ms <= 0 ? 0 : std::min<int64_t>(left_ms, CANCEL_POLL_MS)));
                } while (now_us() < next);
            }
        }
        case OP_DELAY: {
            if (r.a < portTICK_PERIOD_MS) {
                esp_rom_delay_us(r.a * 1000);
                return ESP_OK;
            }
            int64_t until = now_us() + r.a * 1000LL;
            esp_err_t over;
            while ((over = expired(until, cancelled)) == ESP_OK) {
                // Read again after expired(): it may be past by now, and a
                // negative count would become a 49-day uint32_t delay
                int64_t left_ms = (until - now_us()) / 1000;
                if (left_ms > 0) {
                    vTaskDelay(ms_ticks(left_ms < CANCEL_POLL_MS ? left_ms : CANCEL_POLL_MS));
                }
            }
            if (over == ESP_ERR_NOT_FINISHED) {
                return over; // cancelled, even right at the end
            }
            return now_us() >= until && now_us() < g.hook_end_us ? ESP_OK : over;
        }
        case OP_CALL: {
            if (cancelled && cancelled()) {
                return ESP_ERR_NOT_FINISHED;
            }
            int64_t left_ms = (g.hook_end_us - now_us()) / 1000;
            if (left_ms <= 0) {
                return ESP_ERR_TIMEOUT; // past the hook's deadline: don't touch the target
            }
            uint32_t ms = std::min<int64_t>(std::min<int64_t>(r.timeout_ms, left_ms), CALL_MAX_MS);
            return t.call(r.addr, r.args, r.b, ms, cancelled, val);
        }
        case OP_FLASH_READ: {
            esp_err_t ret = t.flash_read(r.addr, g.xfer, r.a);
            *val = r.a;
            return ret;
        }
        case OP_FLASH_WRITE:
            return t.flash_write(r.addr, g.xfer, r.a);
        case OP_FLASH_ERASE:
            return t.flash_erase(r.addr, r.a);
        case OP_RESUME:
            return t.resume();
        case OP_READ_REG:
            return t.read_reg(r.addr, val);
        case OP_WRITE_REG:
            return t.write_reg(r.addr, r.a);
        default:
            return ESP_ERR_NOT_SUPPORTED;
        }
    }

    void priv::post_async(uint8_t slot, uint32_t seq, esp_err_t ret, uint32_t val, const char *why)
    {
        if (slot >= MAX_ASYNC) {
            return;
        }
        async_slot &s = g.slots[slot];
        xSemaphoreTake(g.loop_lock, portMAX_DELAY);
        if (g.loop && s.used && s.seq == seq) {
            s.ret = ret;
            s.val = val;
            strlcpy(s.why, why ? why : "", sizeof(s.why));
            if (qjs_esp_loop_post(g.loop, async_done, &s, pdMS_TO_TICKS(10)) != ESP_OK) {
                ESP_LOGW(TAG, "JS queue full, async result dropped");
            }
        }
        xSemaphoreGive(g.loop_lock);
    }

    esp_err_t priv::execute(const req &r, js_target *t, cancel_fn cancelled)
    {
        esp_err_t ret = ESP_ERR_INVALID_STATE; // load: no target
        uint32_t val = 0;
        char why[sizeof(async_slot::why)] = "";
        bool checked = true;
        if (t && (r.op == OP_FLASH_READ || r.op == OP_FLASH_WRITE || r.op == OP_FLASH_ERASE)) {
            // Arguments first: a bad call mustn't load the flash algorithm (which resets the target).
            auto op = r.op == OP_FLASH_READ    ? js_target::flash_op::read
                      : r.op == OP_FLASH_WRITE ? js_target::flash_op::write
                                               : js_target::flash_op::erase;
            ret = t->flash_check(op, r.addr, r.a, why, sizeof(why));
            if (ret != ESP_ERR_INVALID_ARG) {
                why[0] = '\0'; // another error fails the call as a TargetError
            } else if (why[0] == '\0') {
                strlcpy(why, "bad address or length", sizeof(why));
            }
            checked = ret == ESP_OK;
        }
        for (int i = 0; t && checked && i <= r.retries; i++) {
            ret = run_op(r, *t, cancelled, &val);
            if (ret == ESP_OK) {
                break;
            }
        }

        if (r.slot == SYNC) {
            res x = {r.seq, ret, val};
            xQueueOverwrite(g.res_q, &x);
            return ret;
        }
        post_async(r.slot, r.seq, ret, val, why);
        return ret;
    }

    static esp_err_t serve(js_target *t, uint32_t timeout_ms, cancel_fn cancelled)
    {
        int64_t end = now_us() + (timeout_ms + GRACE_MS) * 1000LL;
        bool stopped = false;
        for (;;) {
            int64_t left_ms = (end - now_us()) / 1000;
            if (left_ms <= 0) {
                if (!stopped) {
                    stop_loop();
                    stopped = true;
                    end = now_us() + GRACE_MS * 1000LL;
                    continue;
                }
                g.dead = true;
                set_err("script hung, scripts off until reboot");
                ESP_LOGE(TAG, "%s", g.err);
                return ESP_ERR_TIMEOUT;
            }

            req r;
            if (xQueueReceive(g.req_q, &r, ms_ticks(left_ms < CANCEL_POLL_MS ? left_ms : CANCEL_POLL_MS)) != pdTRUE) {
                if (!stopped && cancelled && cancelled()) {
                    ESP_LOGW(TAG, "cancelled, stopping the script");
                    stop_loop();
                    stopped = true;
                }
                continue;
            }
            if (r.op == OP_DONE) {
                return r.ret;
            }
            execute(r, t, cancelled);
        }
    }

    js_host *js_host::instance()
    {
        static js_host h;
        return &h;
    }

    engine_info js_host::engine() const
    {
        return {g.ready ? JS_GetVersion() : "", CONFIG_SI_JS_ARENA_KB, HAVE_NETWORK};
    }

    esp_err_t js_host::init()
    {
        if (g.ready) {
            return ESP_OK;
        }
        esp_reset_reason_t why = esp_reset_reason();
        bool crashed = why == ESP_RST_PANIC || why == ESP_RST_INT_WDT || why == ESP_RST_TASK_WDT || why == ESP_RST_WDT;
        if (crashed && crash_mark.magic == CRASH_MAGIC) {
            memcpy(quarantined, crash_mark.sha256, sizeof(quarantined));
            have_quarantine = true;
            ESP_LOGE(TAG, "the device crashed during a run with script %02x%02x%02x%02x..., refusing it until reboot",
                     quarantined[0], quarantined[1], quarantined[2], quarantined[3]);
        }
        crash_mark.magic = 0;

        g.arena = qjs_esp_arena_new(CONFIG_SI_JS_ARENA_KB * 1024, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        g.cmd_q = xQueueCreate(1, sizeof(uint8_t));
        g.req_q = xQueueCreate(8, sizeof(req));
        g.res_q = xQueueCreate(1, sizeof(res));
        g.ended = xSemaphoreCreateBinary();
        g.loop_lock = xSemaphoreCreateMutex();
        g.xfer = (uint8_t *)heap_caps_malloc(XFER_MAX, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (!g.arena || !g.cmd_q || !g.req_q || !g.res_q || !g.ended || !g.loop_lock || !g.xfer || !session_alloc()) {
            ESP_LOGE(TAG, "init: out of memory, scripts are off");
            return ESP_ERR_NO_MEM;
        }
        if (xTaskCreatePinnedToCore(js_task, "js", CONFIG_SI_JS_TASK_STACK, nullptr, CONFIG_SI_JS_TASK_PRIO, nullptr,
                                    CONFIG_SI_JS_TASK_CORE) != pdPASS) {
            ESP_LOGE(TAG, "init: no room for the JS task (%u bytes of internal RAM free)",
                     (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
            return ESP_ERR_NO_MEM;
        }
        g.ready = true;
        ESP_LOGI(TAG, "init: %d KB arena", CONFIG_SI_JS_ARENA_KB);
        return ESP_OK;
    }

    esp_err_t js_host::begin_run(const char *path, const uint8_t *pinned_sha256, const run_info &info)
    {
        g.have_sha = false;
        FILE *fp = fopen(path, "rb");
        if (!fp) {
            set_err("no script %s", path);
            return ESP_ERR_NOT_FOUND;
        }
        fseek(fp, 0, SEEK_END);
        long len = ftell(fp);
        rewind(fp);
        uint8_t *buf = nullptr;
        esp_err_t ret = ESP_OK;
        if (len <= 0 || len > CONFIG_SI_JS_MAX_SCRIPT_KB * 1024) {
            set_err("script too big or empty");
            ret = ESP_ERR_INVALID_SIZE;
        } else if (!(buf = (uint8_t *)heap_caps_malloc(len, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT))) {
            set_err("no room for the script");
            ret = ESP_ERR_NO_MEM;
        } else if (fread(buf, 1, len, fp) != (size_t)len) {
            set_err("can't read %s", path);
            ret = ESP_FAIL;
        }
        fclose(fp);
        if (ret == ESP_OK) {
            ret = begin_run(buf, len, pinned_sha256, info);
        }
        heap_caps_free(buf); // runtime has its own copy
        return ret;
    }

    esp_err_t js_host::begin_run(const uint8_t *blob, size_t len, const uint8_t *pinned_sha256, const run_info &info)
    {
        end_run();
        g.err[0] = '\0';
        g.js_err_set = false;
        memset(g.hooks, 0, sizeof(g.hooks));

        size_t hash_len = 0;
        g.have_sha = psa_hash_compute(PSA_ALG_SHA_256, blob, len, g.sha256, sizeof(g.sha256), &hash_len) == PSA_SUCCESS;
        g.pinned = pinned_sha256 != nullptr;
        if (!g.ready || g.dead) {
            set_err(g.dead ? "scripts off until reboot" : "scripts unavailable");
            return ESP_ERR_INVALID_STATE;
        }
        if (!g.have_sha) {
            set_err("can't hash the script");
            return ESP_FAIL;
        }
        if (pinned_sha256 && memcmp(pinned_sha256, g.sha256, sizeof(g.sha256)) != 0) {
            set_err("script isn't the one the job pins");
            return ESP_ERR_INVALID_CRC;
        }
        if (have_quarantine && memcmp(quarantined, g.sha256, sizeof(g.sha256)) == 0) {
            set_err("this script crashed the device, reboot to retry");
            return ESP_ERR_INVALID_STATE;
        }
        if (info.network && !HAVE_NETWORK) {
            set_err("job wants the network, this firmware has none for scripts");
            return ESP_ERR_NOT_SUPPORTED;
        }
        g.network = info.network;
        g.prog_last_stage = nullptr;
        g.prog_last_us = 0;
        g.prog_posted = false;

        strlcpy(g.job_name, info.job_name ? info.job_name : "", sizeof(g.job_name));
        strlcpy(g.variant, info.variant ? info.variant : "", sizeof(g.variant));
        strlcpy(g.family, info.family ? info.family : "", sizeof(g.family));
        if (!claim(USER_RUN)) {
            set_err("an agent session is using scripts");
            return ESP_ERR_INVALID_STATE;
        }
        g.script = blob;
        g.script_len = len;
        xQueueReset(g.req_q);
        xQueueReset(g.res_q);
        xSemaphoreTake(g.ended, 0);

        memcpy(crash_mark.sha256, g.sha256, sizeof(crash_mark.sha256));
        crash_mark.magic = CRASH_MAGIC;
        g.running = true;
        g.hook_end_us = now_us() + LOAD_TIMEOUT_MS * 1000LL;

        uint8_t cmd = CMD_RUN;
        xQueueSend(g.cmd_q, &cmd, portMAX_DELAY);
        esp_err_t ret = serve(nullptr, LOAD_TIMEOUT_MS, nullptr);
        g.script = nullptr;
        if (ret != ESP_OK && g.err[0] == '\0') {
            set_err("script failed to load");
        }
        return ret;
    }

    bool js_host::has_hook(hook h) const
    {
        return g.running && g.alive && !g.dead && g.hooks[(int)h];
    }

    bool js_host::exports_hook(hook h) const
    {
        return g.running && g.hooks[(int)h];
    }

    esp_err_t js_host::run_hook(hook h, js_target &target, uint32_t timeout_ms, cancel_fn cancelled)
    {
        if (!g.running || g.dead) {
            return ESP_ERR_INVALID_STATE;
        }
        g.cur_hook = h;
        g.cur_timeout_ms = timeout_ms ? timeout_ms : CONFIG_SI_JS_HOOK_TIMEOUT_MS;
        g.hook_end_us = now_us() + g.cur_timeout_ms * 1000LL;
        xQueueReset(g.res_q);

        xSemaphoreTake(g.loop_lock, portMAX_DELAY);
        bool posted = g.loop && g.alive && qjs_esp_loop_post(g.loop, call_hook, nullptr, pdMS_TO_TICKS(100)) == ESP_OK;
        xSemaphoreGive(g.loop_lock);
        if (!posted) {
            if (strncmp(g.err, ENDED_BETWEEN, strlen(ENDED_BETWEEN)) != 0) { // else keep why it ended
                set_err("script isn't running");
            }
            return ESP_ERR_INVALID_STATE;
        }
        return serve(&target, g.cur_timeout_ms, cancelled);
    }

    esp_err_t js_host::run_on_fail(const char *stage, const char *reason, js_target &target, uint32_t timeout_ms,
                                   cancel_fn cancelled)
    {
        if (!has_hook(hook::on_fail)) {
            return ESP_ERR_NOT_FOUND;
        }
        char err[sizeof(g.err)];
        strlcpy(err, g.err, sizeof(err));
        strlcpy(g.fail_stage, stage ? stage : "", sizeof(g.fail_stage));
        g.fail_js = false;
        if (reason == nullptr || reason[0] == '\0') {
            reason = err[0] ? err : "failed";
            g.fail_js = err[0] && g.js_err_set;
        }
        strlcpy(g.fail_reason, reason, sizeof(g.fail_reason));
        bool js_err_set = g.js_err_set;
        esp_err_t ret = run_hook(hook::on_fail, target, timeout_ms, cancelled);
        if (ret != ESP_OK) {
            ESP_LOGW(TAG, "onFail failed too: %s", g.err);
        }
        strlcpy(g.err, err, sizeof(g.err)); // the run's error stays the first failure
        g.js_err_set = js_err_set;
        return ret;
    }

    void js_host::end_run()
    {
        if (!g.running) {
            return;
        }
        g.running = false;
        if (!g.dead) {
            stop_loop();
            if (xSemaphoreTake(g.ended, pdMS_TO_TICKS(GRACE_MS)) != pdTRUE) {
                g.dead = true;
                ESP_LOGE(TAG, "JS task didn't end the run, scripts off until reboot");
            }
        }
        crash_mark.magic = 0;
        g.user = USER_NONE;
    }

    bool js_host::last_script(uint8_t sha256[32], bool *pinned) const
    {
        if (!g.have_sha) {
            return false;
        }
        memcpy(sha256, g.sha256, sizeof(g.sha256));
        if (pinned) {
            *pinned = g.pinned;
        }
        return true;
    }

    void js_host::forget_last_script()
    {
        g.have_sha = false;
        g.pinned = false;
        g.results_json[0] = '\0';
    }

    const char *js_host::last_results() const
    {
        return g.results_json;
    }

    void js_host::progress(const char *stage, int pct)
    {
        if (!g.running || g.dead) {
            return;
        }
        g.prog_stage = stage;
        g.prog_pct = pct;
        int64_t now = now_us();
        bool news = stage != g.prog_last_stage || pct >= 100 || now - g.prog_last_us >= 100000;
        if (!news || g.prog_posted.exchange(true)) {
            return;
        }
        g.prog_last_stage = stage;
        g.prog_last_us = now;
        xSemaphoreTake(g.loop_lock, portMAX_DELAY);
        bool posted = g.loop && qjs_esp_loop_post(g.loop, progress_cb, nullptr, 0) == ESP_OK;
        xSemaphoreGive(g.loop_lock);
        if (!posted) {
            g.prog_posted = false;
        }
    }

    const char *js_host::last_error() const
    {
        return g.err;
    }
} // namespace qjs_binder

#else // stubs: no QuickJS linked

namespace qjs_binder
{
    js_host *js_host::instance()
    {
        static js_host h;
        return &h;
    }
    engine_info js_host::engine() const { return {"", 0, false}; }
    esp_err_t js_host::init() { return ESP_ERR_NOT_SUPPORTED; }
    esp_err_t js_host::begin_run(const char *, const uint8_t *, const run_info &) { return ESP_ERR_NOT_SUPPORTED; }
    esp_err_t js_host::begin_run(const uint8_t *, size_t, const uint8_t *, const run_info &) { return ESP_ERR_NOT_SUPPORTED; }
    bool js_host::has_hook(hook) const { return false; }
    bool js_host::exports_hook(hook) const { return false; }
    esp_err_t js_host::run_hook(hook, js_target &, uint32_t, cancel_fn) { return ESP_ERR_NOT_SUPPORTED; }
    esp_err_t js_host::run_on_fail(const char *, const char *, js_target &, uint32_t, cancel_fn) { return ESP_ERR_NOT_SUPPORTED; }
    void js_host::end_run() {}
    bool js_host::last_script(uint8_t[32], bool *) const { return false; }
    void js_host::forget_last_script() {}
    const char *js_host::last_results() const { return ""; }
    void js_host::progress(const char *, int) {}
    const char *js_host::last_error() const { return "this firmware doesn't run scripts"; }
} // namespace qjs_binder

#endif
