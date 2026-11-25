#include <cstring>

#include <esp_log.h>
#include <esp_random.h>
#include <psa/crypto.h>

#include "host_priv.hpp"

#if CONFIG_SI_JS_ENABLE

namespace qjs_binder::priv
{
    static const char *TAG = "js_host";

    static JSValue js_random_bytes(JSContext *ctx, JSValueConst, int argc, JSValueConst *argv)
    {
        uint32_t n;
        if (JS_ToUint32(ctx, &n, argv[0])) {
            return JS_EXCEPTION;
        }
        if (n > XFER_MAX) {
            return JS_ThrowRangeError(ctx, "randomBytes: at most %u bytes", (unsigned)XFER_MAX);
        }
        uint8_t *buf = (uint8_t *)js_malloc(ctx, n ? n : 1);
        if (!buf) {
            return JS_EXCEPTION;
        }
        esp_fill_random(buf, n);
        JSValue v = JS_NewUint8ArrayCopy(ctx, buf, n);
        js_free(ctx, buf);
        return v;
    }

    static JSValue js_sha256(JSContext *ctx, JSValueConst, int argc, JSValueConst *argv)
    {
        const uint8_t *p;
        size_t len;
        const char *str = nullptr;
        if (JS_IsString(argv[0])) {
            str = JS_ToCStringLen(ctx, &len, argv[0]); // UTF-8
            if (!str) {
                return JS_EXCEPTION;
            }
            p = (const uint8_t *)str;
        } else if (get_bytes(ctx, argv[0], &p, &len)) {
            return JS_EXCEPTION;
        }
        uint8_t out[32];
        size_t out_len = 0;
        psa_status_t st = psa_hash_compute(PSA_ALG_SHA_256, p, len, out, sizeof(out), &out_len);
        JS_FreeCString(ctx, str);
        if (st != PSA_SUCCESS) {
            return JS_ThrowInternalError(ctx, "sha256 failed (%d)", (int)st);
        }
        return JS_NewUint8ArrayCopy(ctx, out, sizeof(out));
    }

    static JSValue commit_results(JSContext *ctx)
    {
        JSValue s = JS_JSONStringify(ctx, g.results, JS_UNDEFINED, JS_UNDEFINED);
        if (JS_IsException(s)) {
            return s;
        }
        size_t len = 0;
        const char *str = JS_ToCStringLen(ctx, &len, s);
        JS_FreeValue(ctx, s);
        if (!str) {
            return JS_EXCEPTION;
        }
        if (len <= RESULTS_MAX) {
            memcpy(g.results_json, str, len + 1);
            JS_FreeCString(ctx, str);
            return JS_UNDEFINED;
        }
        JS_FreeCString(ctx, str);
        JS_FreeValue(ctx, g.results);
        const char *last = g.results_json[0] ? g.results_json : "{}";
        g.results = JS_ParseJSON(ctx, last, strlen(last), "<results>");
        return JS_ThrowRangeError(ctx, "results would be %zu bytes of JSON, at most %u fit the run record", len,
                                  (unsigned)RESULTS_MAX);
    }

    static JSValue js_record(JSContext *ctx, JSValueConst, int argc, JSValueConst *argv)
    {
        if (!JS_IsObject(argv[0])) {
            return JS_ThrowTypeError(ctx, "record: expected an object");
        }
        JSPropertyEnum *tab = nullptr;
        uint32_t n = 0;
        if (JS_GetOwnPropertyNames(ctx, &tab, &n, argv[0], JS_GPN_STRING_MASK | JS_GPN_ENUM_ONLY)) {
            return JS_EXCEPTION;
        }
        int bad = 0;
        for (uint32_t i = 0; i < n && !bad; i++) {
            JSValue v = JS_GetProperty(ctx, argv[0], tab[i].atom);
            bad = JS_IsException(v) || JS_SetProperty(ctx, g.results, tab[i].atom, v) < 0;
        }
        JS_FreePropertyEnum(ctx, tab, n);
        return bad ? JS_EXCEPTION : commit_results(ctx);
    }

    static JSValue js_check(JSContext *ctx, JSValueConst, int argc, JSValueConst *argv)
    {
        double v, lo = 0, hi = 0;
        const char *name = JS_ToCString(ctx, argv[0]);
        if (!name || JS_ToFloat64(ctx, &v, argv[1])) {
            JS_FreeCString(ctx, name);
            return JS_EXCEPTION;
        }
        JSValueConst lim = argc > 2 ? argv[2] : JS_UNDEFINED;
        JSValue jlo = JS_IsObject(lim) ? JS_GetPropertyStr(ctx, lim, "lo") : JS_UNDEFINED;
        JSValue jhi = JS_IsObject(lim) ? JS_GetPropertyStr(ctx, lim, "hi") : JS_UNDEFINED;
        JSValue unit = JS_IsObject(lim) ? JS_GetPropertyStr(ctx, lim, "unit") : JS_UNDEFINED;
        bool has_lo = !JS_IsUndefined(jlo), has_hi = !JS_IsUndefined(jhi);
        if ((has_lo && JS_ToFloat64(ctx, &lo, jlo)) || (has_hi && JS_ToFloat64(ctx, &hi, jhi))) {
            JS_FreeValue(ctx, jlo);
            JS_FreeValue(ctx, jhi);
            JS_FreeValue(ctx, unit);
            JS_FreeCString(ctx, name);
            return JS_EXCEPTION;
        }
        bool pass = (!has_lo || v >= lo) && (!has_hi || v <= hi);

        JSValue e = JS_NewObject(ctx);
        JS_SetPropertyStr(ctx, e, "value", JS_NewFloat64(ctx, v));
        if (has_lo) {
            JS_SetPropertyStr(ctx, e, "lo", JS_NewFloat64(ctx, lo));
        }
        if (has_hi) {
            JS_SetPropertyStr(ctx, e, "hi", JS_NewFloat64(ctx, hi));
        }
        if (!JS_IsUndefined(unit)) {
            JS_SetPropertyStr(ctx, e, "unit", JS_DupValue(ctx, unit));
        }
        JS_SetPropertyStr(ctx, e, "pass", JS_NewBool(ctx, pass));
        JS_SetPropertyStr(ctx, g.results, name, e);
        JS_FreeValue(ctx, jlo);
        JS_FreeValue(ctx, jhi);

        JSValue ret = commit_results(ctx);
        if (!JS_IsException(ret) && !pass) {
            const char *u = JS_IsString(unit) ? JS_ToCString(ctx, unit) : nullptr;
            ret = JS_ThrowRangeError(ctx, "%s = %g%s is out of limits", name, v, u ? u : "");
            JS_FreeCString(ctx, u);
        }
        JS_FreeValue(ctx, unit);
        JS_FreeCString(ctx, name);
        return JS_IsException(ret) ? ret : JS_TRUE;
    }

    static JSValue js_on_progress(JSContext *ctx, JSValueConst, int argc, JSValueConst *argv)
    {
        if (!JS_IsFunction(ctx, argv[0]) && !JS_IsNull(argv[0]) && !JS_IsUndefined(argv[0])) {
            return JS_ThrowTypeError(ctx, "onProgress: expected a function or null");
        }
        JS_FreeValue(ctx, g.on_progress);
        g.on_progress = JS_DupValue(ctx, argv[0]);
        return JS_UNDEFINED;
    }

    void progress_cb(JSContext *ctx, void *)
    {
        g.prog_posted = false; // anything newer posts again
        if (!JS_IsFunction(ctx, g.on_progress)) {
            return;
        }
        JSValue p = JS_NewObject(ctx);
        JS_SetPropertyStr(ctx, p, "stage", JS_NewString(ctx, g.prog_stage.load()));
        JS_SetPropertyStr(ctx, p, "pct", JS_NewInt32(ctx, g.prog_pct.load()));
        JSValue fn = JS_DupValue(ctx, g.on_progress); // it may replace itself
        JSValue r = JS_Call(ctx, fn, JS_UNDEFINED, 1, &p);
        if (JS_IsException(r)) {
            JSValue e = JS_GetException(ctx);
            if (!ends_runtime(e)) {
                const char *msg = JS_ToCString(ctx, e);
                ESP_LOGW(TAG, "onProgress threw: %s", msg ? msg : "?");
                JS_FreeCString(ctx, msg);
            }
            JS_FreeValue(ctx, e);
        }
        JS_FreeValue(ctx, r);
        JS_FreeValue(ctx, fn);
        JS_FreeValue(ctx, p);
    }

    static const JSCFunctionListEntry crypto_funcs[] = {
        JS_CFUNC_DEF("randomBytes", 1, js_random_bytes),
        JS_CFUNC_DEF("sha256", 1, js_sha256),
    };
    static const JSCFunctionListEntry log_funcs[] = {
        JS_CFUNC_DEF("record", 1, js_record),
        JS_CFUNC_DEF("check", 3, js_check),
    };
    static const JSCFunctionListEntry job_funcs[] = {
        JS_CFUNC_DEF("onProgress", 1, js_on_progress),
    };

    template <const JSCFunctionListEntry *tab, size_t n> static int module_init(JSContext *ctx, JSModuleDef *m)
    {
        return JS_SetModuleExportList(ctx, m, tab, n);
    }

    template <const JSCFunctionListEntry *tab, size_t n> static bool add_module(JSContext *ctx, const char *name)
    {
        JSModuleDef *m = JS_NewCModule(ctx, name, module_init<tab, n>);
        return m && JS_AddModuleExportList(ctx, m, tab, n) == 0;
    }

#define ADD(ctx, name, tab) add_module<tab, sizeof(tab) / sizeof(tab[0])>(ctx, name)

    bool init_misc_modules(JSContext *ctx, bool session)
    {
        if (session) {
            return ADD(ctx, "si:crypto", crypto_funcs) && init_agent_module(ctx);
        }
        return ADD(ctx, "si:crypto", crypto_funcs) && ADD(ctx, "si:log", log_funcs) && ADD(ctx, "si:job", job_funcs);
    }

    bool add_misc_globals(JSContext *ctx, JSValueConst global)
    {
        JSValue o = JS_NewObject(ctx);
        if (JS_IsException(o) || JS_SetPropertyFunctionList(ctx, o, crypto_funcs, sizeof(crypto_funcs) / sizeof(crypto_funcs[0])) < 0) {
            JS_FreeValue(ctx, o);
            return false;
        }
        return JS_SetPropertyStr(ctx, global, "crypto", o) >= 0 && add_agent_global(ctx, global);
    }
} // namespace qjs_binder::priv

#endif
