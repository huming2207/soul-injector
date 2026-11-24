/*
 * JS task, run lifecycle, hooks, and the owner side that serves target calls.
 * See host_priv.hpp for who talks to whom.
 */

#include <cstdio>
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

    static constexpr uint32_t LOAD_TIMEOUT_MS = 5000;
    static constexpr uint32_t GRACE_MS = 2000;      // JS task gets this long past a deadline to come back
    static constexpr uint32_t CANCEL_POLL_MS = 50;
    static constexpr const char *HOOK_NAMES[] = {"preProgram", "postProgram"};

    /*
     * The script of the run in progress. It survives a panic or watchdog reset
     * (not a power cycle), so the next boot refuses that script instead of
     * crashing again on every plugged-in target.
     */
    static __NOINIT_ATTR struct {
        uint32_t magic;
        uint8_t sha256[32];
    } crash_mark;
    static constexpr uint32_t CRASH_MAGIC = 0x4a53524e;
    static uint8_t quarantined[32];
    static bool have_quarantine;

    static int64_t now_us()
    {
        return esp_timer_get_time();
    }

    static void set_err(const char *fmt, const char *arg = "")
    {
        snprintf(g.err, sizeof(g.err), fmt, arg);
    }

    // ------------------------------------------------------------------
    // JS task
    // ------------------------------------------------------------------

    /* Log an error value with its stack; keep one line for the display */
    static void record_error(JSContext *ctx, JSValueConst e)
    {
        const char *msg = JS_ToCString(ctx, e);
        JSValue stack = JS_IsError(e) ? JS_GetPropertyStr(ctx, e, "stack") : JS_UNDEFINED;
        const char *st = JS_IsString(stack) ? JS_ToCString(ctx, stack) : nullptr;
        ESP_LOGE(TAG, "script: %s\n%s", msg ? msg : "(error)", st ? st : "");
        snprintf(g.err, sizeof(g.err), "%s", msg ? msg : "script error");
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
            ESP_LOGE(TAG, "owner queue full, result lost"); // the owner's deadline catches it
        }
    }

    /* The load or a hook settled. Only the first report counts. */
    static void finish(JSContext *ctx, esp_err_t ret)
    {
        if (!g.hook_active && !g.loading) {
            return;
        }
        if (g.loading && ret == ESP_OK) {
            JSValue ns = JS_GetModuleNamespace(ctx, g.mod);
            for (int i = 0; i < 2; i++) {
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
        if (magic && argc && JS_IsUncatchableError(argv[0])) {
            return JS_UNDEFINED; // stopped, see settle()
        }
        if (magic) {
            record_error(ctx, argc ? argv[0] : JS_UNDEFINED);
        }
        finish(ctx, magic ? ESP_FAIL : ESP_OK);
        return JS_UNDEFINED;
    }

    /* Report when r settles; r is consumed */
    static void settle(JSContext *ctx, JSValue r)
    {
        if (JS_IsException(r)) {
            JSValue e = JS_GetException(ctx);
            // Uncatchable = the loop was stopped (slice, deadline, cancel); it
            // reports that itself with the reason once it returns
            bool stopped = JS_IsUncatchableError(e);
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
        JSValue p = JS_PromiseThen(ctx, r, ok, bad);
        if (JS_IsException(p)) {
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

    /* Imports resolve to the modules we registered, nothing else */
    static JSModuleDef *refuse_module(JSContext *ctx, const char *name, void *)
    {
        JS_ThrowReferenceError(ctx, "no module '%s' here (bundle local files into the script)", name);
        return nullptr;
    }

    /*
     * No eval/Function (bytecode only), no Proxy or WeakRef, no qjs:std or
     * qjs:os (files, environment). The target is si:target.
     */
    static JSContext *new_context(JSRuntime *rt)
    {
        JSContext *ctx = JS_NewContextRaw(rt);
        if (!ctx) {
            return nullptr;
        }
        if (JS_AddIntrinsicBaseObjects(ctx) || JS_AddIntrinsicDate(ctx) || JS_AddIntrinsicRegExp(ctx) ||
            JS_AddIntrinsicJSON(ctx) || JS_AddIntrinsicMapSet(ctx) || JS_AddIntrinsicTypedArrays(ctx) ||
            JS_AddIntrinsicPromise(ctx) || JS_AddIntrinsicAToB(ctx) || JS_AddPerformance(ctx) ||
            !init_target_module(ctx)) {
            JS_FreeContext(ctx);
            return nullptr;
        }
        JSValue global = JS_GetGlobalObject(ctx);
        JSValue console = JS_NewObject(ctx);
        JS_SetPropertyFunctionList(ctx, console, console_funcs, sizeof(console_funcs) / sizeof(console_funcs[0]));
        JS_SetPropertyStr(ctx, global, "console", console);
        JS_FreeValue(ctx, global);
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

    /* qjs_esp_loop_post from the owner: call the current hook */
    static void call_hook(JSContext *ctx, void *)
    {
        g.hook_active = true;
        qjs_esp_loop_set_deadline(g.loop, now_us() + g.cur_timeout_ms * 1000LL);
        qjs_esp_loop_set_limits(g.loop, &g.lim); // fresh slice for the hook

        JSValue ns = JS_GetModuleNamespace(ctx, g.mod);
        JSValue fn = JS_GetPropertyStr(ctx, ns, HOOK_NAMES[(int)g.cur_hook]);
        JSValue arg = job_info(ctx);
        JSValue r = JS_IsFunction(ctx, fn) ? JS_Call(ctx, fn, JS_UNDEFINED, 1, &arg)
                                           : JS_ThrowTypeError(ctx, "no %s hook", HOOK_NAMES[(int)g.cur_hook]);
        JS_FreeValue(ctx, arg);
        JS_FreeValue(ctx, fn);
        JS_FreeValue(ctx, ns);
        settle(ctx, r);
    }

    static const char *stop_text(qjs_esp_stop_t why)
    {
        switch (why) {
        case QJS_ESP_STOP_DEADLINE:
            return "script ran out of time";
        case QJS_ESP_STOP_SLICE:
            return "script ran too long without yielding";
        case QJS_ESP_STOP_REQUESTED:
            return "script stopped";
        default:
            return "script ended";
        }
    }

    static void run_script()
    {
        JSRuntime *rt = qjs_esp_new_runtime_in(g.arena);
        if (!rt) {
            qjs_esp_arena_reset(g.arena); // a leftover means a teardown went wrong; it's all garbage
            rt = qjs_esp_new_runtime_in(g.arena);
        }
        JSContext *ctx = rt ? (js_std_init_handlers(rt), new_context(rt)) : nullptr;
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
        qjs_esp_loop_keep_alive(loop, true); // hooks arrive later as posted work
        qjs_esp_loop_set_deadline(loop, now_us() + LOAD_TIMEOUT_MS * 1000LL);
        xSemaphoreTake(g.loop_lock, portMAX_DELAY);
        g.loop = loop;
        xSemaphoreGive(g.loop_lock);

        g.loading = true;
        g.hooks[0] = g.hooks[1] = false;
        settle(ctx, qjs_esp_bc_eval(ctx, g.script, g.script_len, &g.mod));

        // Hooks run inside this. Only a stop gets us out: end_run, cancel, a deadline or a slice.
        qjs_esp_stop_t why = qjs_esp_loop_run(loop);
        if (g.loading || g.hook_active) {
            set_err("%s", stop_text(why));
            ESP_LOGE(TAG, "%s", g.err);
            g.loading = g.hook_active = false;
            send_done(why == QJS_ESP_STOP_REQUESTED ? ESP_ERR_INVALID_STATE : ESP_ERR_TIMEOUT);
        }

        xSemaphoreTake(g.loop_lock, portMAX_DELAY);
        qjs_esp_loop_free(loop); // stops nothing here (no I/O modules), drains posted work
        g.loop = nullptr;
        xSemaphoreGive(g.loop_lock);
        memset(g.slots, 0, sizeof(g.slots));
        g.mod = nullptr;
        // Everything the script made lives in the arena: drop it whole. A runtime
        // stopped mid-call may not free cleanly, and JS_FreeRuntime asserts on leaks.
        qjs_esp_arena_reset(g.arena);
    }

    static void js_task(void *)
    {
        for (;;) {
            uint8_t cmd;
            xQueueReceive(g.cmd_q, &cmd, portMAX_DELAY);
            run_script();
            xSemaphoreGive(g.ended);
        }
    }

    // ------------------------------------------------------------------
    // Owner: target calls
    // ------------------------------------------------------------------

    static TickType_t ms_ticks(uint32_t ms)
    {
        TickType_t t = pdMS_TO_TICKS(ms);
        return t ? t : 1;
    }

    static void stop_loop()
    {
        xSemaphoreTake(g.loop_lock, portMAX_DELAY);
        if (g.loop) {
            qjs_esp_loop_stop(g.loop);
        }
        xSemaphoreGive(g.loop_lock);
    }

    /* Async ops never outlive the hook, nor a cancel: ESP_OK while there's time */
    static esp_err_t expired(int64_t until_us, cancel_fn cancelled)
    {
        if (cancelled && cancelled()) {
            return ESP_ERR_INVALID_STATE; // STOPPED
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
                esp_err_t over = expired(until, cancelled);
                if (over != ESP_OK) {
                    return over;
                }
                vTaskDelay(ms_ticks(r.interval_ms));
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
                int64_t left_ms = (until - now_us()) / 1000;
                vTaskDelay(ms_ticks(left_ms < CANCEL_POLL_MS ? left_ms : CANCEL_POLL_MS));
            }
            // reaching the delay's own end is the point, running past the hook isn't
            return now_us() >= until && now_us() < g.hook_end_us ? ESP_OK : over;
        }
        default:
            return ESP_ERR_NOT_SUPPORTED;
        }
    }

    static void execute(const req &r, js_target *t, cancel_fn cancelled)
    {
        esp_err_t ret = ESP_ERR_INVALID_STATE; // no target: called during the load
        uint32_t val = 0;
        for (int i = 0; t && i <= r.retries; i++) {
            ret = run_op(r, *t, cancelled, &val);
            if (ret == ESP_OK) {
                break;
            }
        }

        if (r.slot == SYNC) {
            res x = {r.seq, ret, val};
            xQueueOverwrite(g.res_q, &x);
            return;
        }
        if (r.slot >= MAX_ASYNC) {
            return;
        }
        async_slot &s = g.slots[r.slot];
        s.ret = ret;
        s.val = val;
        xSemaphoreTake(g.loop_lock, portMAX_DELAY);
        // A JS task with a full queue is buried; its deadline sorts that out
        if (g.loop && qjs_esp_loop_post(g.loop, async_done, &s, pdMS_TO_TICKS(10)) != ESP_OK) {
            ESP_LOGW(TAG, "JS queue full, async result dropped");
        }
        xSemaphoreGive(g.loop_lock);
    }

    /*
     * Serve target calls until the script reports OP_DONE. The script's own
     * deadline is timeout_ms; past that plus GRACE_MS we stop the loop, and if
     * the JS task still doesn't answer it's stuck in native code: scripting is
     * off until reboot, the caller fails the run and goes on.
     */
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

    // ------------------------------------------------------------------
    // js_host
    // ------------------------------------------------------------------

    js_host *js_host::instance()
    {
        static js_host h;
        return &h;
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
        if (!g.arena || !g.cmd_q || !g.req_q || !g.res_q || !g.ended || !g.loop_lock) {
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
        heap_caps_free(buf); // the runtime has its own copy once loaded
        return ret;
    }

    esp_err_t js_host::begin_run(const uint8_t *blob, size_t len, const uint8_t *pinned_sha256, const run_info &info)
    {
        end_run();
        g.err[0] = '\0';
        g.hooks[0] = g.hooks[1] = false;

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

        strlcpy(g.job_name, info.job_name ? info.job_name : "", sizeof(g.job_name));
        strlcpy(g.variant, info.variant ? info.variant : "", sizeof(g.variant));
        strlcpy(g.family, info.family ? info.family : "", sizeof(g.family));
        g.script = blob;
        g.script_len = len;
        xQueueReset(g.req_q);
        xQueueReset(g.res_q);
        xSemaphoreTake(g.ended, 0);

        memcpy(crash_mark.sha256, g.sha256, sizeof(crash_mark.sha256));
        crash_mark.magic = CRASH_MAGIC;
        g.running = true;
        g.hook_end_us = now_us() + LOAD_TIMEOUT_MS * 1000LL;

        uint8_t cmd = 1;
        xQueueSend(g.cmd_q, &cmd, portMAX_DELAY); // empty: the last run ended
        esp_err_t ret = serve(nullptr, LOAD_TIMEOUT_MS, nullptr);
        g.script = nullptr;
        if (ret != ESP_OK && g.err[0] == '\0') {
            set_err("script failed to load");
        }
        return ret;
    }

    bool js_host::has_hook(hook h) const
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
        bool posted = g.loop && qjs_esp_loop_post(g.loop, call_hook, nullptr, pdMS_TO_TICKS(100)) == ESP_OK;
        xSemaphoreGive(g.loop_lock);
        if (!posted) {
            set_err("script isn't running");
            return ESP_ERR_INVALID_STATE;
        }
        return serve(&target, g.cur_timeout_ms, cancelled);
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
    }

    const char *js_host::last_error() const
    {
        return g.err;
    }
} // namespace qjs_binder

#else // !CONFIG_SI_JS_ENABLE: stubs, so none of QuickJS ends up in the image

namespace qjs_binder
{
    js_host *js_host::instance()
    {
        static js_host h;
        return &h;
    }
    esp_err_t js_host::init() { return ESP_ERR_NOT_SUPPORTED; }
    esp_err_t js_host::begin_run(const char *, const uint8_t *, const run_info &) { return ESP_ERR_NOT_SUPPORTED; }
    esp_err_t js_host::begin_run(const uint8_t *, size_t, const uint8_t *, const run_info &) { return ESP_ERR_NOT_SUPPORTED; }
    bool js_host::has_hook(hook) const { return false; }
    esp_err_t js_host::run_hook(hook, js_target &, uint32_t, cancel_fn) { return ESP_ERR_NOT_SUPPORTED; }
    void js_host::end_run() {}
    bool js_host::last_script(uint8_t[32], bool *) const { return false; }
    void js_host::forget_last_script() {}
    const char *js_host::last_error() const { return "this firmware doesn't run scripts"; }
} // namespace qjs_binder

#endif
