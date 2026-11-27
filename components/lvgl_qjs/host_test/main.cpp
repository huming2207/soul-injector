// Host test of lvgl_qjs: runs a test script against LVGL with a dummy
// display and a fake qjs_esp loop.
//
// The script exports `steps`, an array of functions. Each runs in turn, then
// the loop runs dry (event dispatches, drains), as it would on the device
// between two turns. A global `host` plays the LVGL task's part:
//   host.send(obj, "click" | "change" | "press")  an input event, as LVGL sends it
//   host.queued()          work waiting in the loop
//   host.detached()        [objects in limbo, objects in trash]
//   host.cleanRoot()       the host empties lv.root() from C
//   host.gc()              JS_RunGC
//   host.nodes()           wrappers the binding tracks
// Then the runtime is torn down as a host does it; QuickJS asserts that no
// object leaked and AddressSanitizer catches the rest.

#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "lvgl_qjs_priv.hpp"
#include "quickjs-libc.h"

using lvgl_qjs::priv::g;

// ---- A fake qjs_esp loop: a queue of posted work, run until empty ----

struct qjs_esp_loop {
    struct work {
        qjs_esp_work_cb_t cb;
        void *arg;
    };
    static constexpr size_t QUEUE = 32;
    JSContext *ctx;
    work q[QUEUE];
    size_t head;
    size_t count;
    bool failed;
};

esp_err_t qjs_esp_loop_post(qjs_esp_loop_t *l, qjs_esp_work_cb_t cb, void *arg, TickType_t)
{
    if (l->count == qjs_esp_loop::QUEUE) {
        return ESP_ERR_TIMEOUT;
    }
    l->q[(l->head + l->count++) % qjs_esp_loop::QUEUE] = {cb, arg};
    return ESP_OK;
}

void qjs_esp_loop_fail(qjs_esp_loop_t *l, JSValueConst)
{
    l->failed = true;
}

static void run_loop(qjs_esp_loop_t *l)
{
    JSRuntime *rt = JS_GetRuntime(l->ctx);
    for (;;) {
        JSContext *c;
        while (JS_ExecutePendingJob(rt, &c) > 0) {
        }
        if (!l->count) {
            return;
        }
        qjs_esp_loop::work w = l->q[l->head];
        l->head = (l->head + 1) % qjs_esp_loop::QUEUE;
        l->count--;
        w.cb(l->ctx, w.arg);
    }
}

// ---- LVGL lock: single threaded here, but it must balance ----

static int lock_depth = 0;

static bool test_lock(uint32_t)
{
    lock_depth++;
    return true;
}

static void test_unlock()
{
    lock_depth--;
}

static void flush_cb(lv_display_t *disp, const lv_area_t *, uint8_t *)
{
    lv_display_flush_ready(disp);
}

// ---- Checks ----

static int failures = 0;

#define CHECK(cond)                                                         \
    do {                                                                    \
        if (!(cond)) {                                                      \
            fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
            failures++;                                                     \
        }                                                                   \
    } while (0)

static size_t node_count()
{
    size_t n = 0;
    for (lvgl_qjs::priv::node *p = g.nodes; p; p = p->next) {
        n++;
    }
    return n;
}

// ---- The `host` global ----

static lv_obj_t *test_root;
static qjs_esp_loop_t *test_loop;

static JSValue host_send(JSContext *ctx, JSValueConst, int, JSValueConst *argv)
{
    lv_obj_t *o = lvgl_qjs::priv::obj_of(ctx, argv[0]);
    const char *what = o ? JS_ToCString(ctx, argv[1]) : nullptr;
    if (!what) {
        return JS_EXCEPTION;
    }
    const lv_event_code_t code = strcmp(what, "click") == 0    ? LV_EVENT_CLICKED
                                 : strcmp(what, "change") == 0 ? LV_EVENT_VALUE_CHANGED
                                                               : LV_EVENT_PRESSED;
    JS_FreeCString(ctx, what);
    lv_obj_send_event(o, code, nullptr);
    return JS_UNDEFINED;
}

static JSValue host_queued(JSContext *ctx, JSValueConst, int, JSValueConst *)
{
    return JS_NewUint32(ctx, test_loop->count);
}

static JSValue host_detached(JSContext *ctx, JSValueConst, int, JSValueConst *)
{
    JSValue a = JS_NewArray(ctx);
    JS_SetPropertyUint32(ctx, a, 0, JS_NewUint32(ctx, lv_obj_get_child_count(g.limbo)));
    JS_SetPropertyUint32(ctx, a, 1, JS_NewUint32(ctx, lv_obj_get_child_count(g.trash)));
    return a;
}

static JSValue host_clean_root(JSContext *, JSValueConst, int, JSValueConst *)
{
    lv_obj_clean(test_root);
    return JS_UNDEFINED;
}

static JSValue host_gc(JSContext *ctx, JSValueConst, int, JSValueConst *)
{
    JS_RunGC(JS_GetRuntime(ctx));
    return JS_UNDEFINED;
}

static JSValue host_nodes(JSContext *ctx, JSValueConst, int, JSValueConst *)
{
    return JS_NewUint32(ctx, node_count());
}

static const JSCFunctionListEntry HOST_FUNCS[] = {
    JS_CFUNC_DEF("send", 2, host_send),
    JS_CFUNC_DEF("queued", 0, host_queued),
    JS_CFUNC_DEF("detached", 0, host_detached),
    JS_CFUNC_DEF("cleanRoot", 0, host_clean_root),
    JS_CFUNC_DEF("gc", 0, host_gc),
    JS_CFUNC_DEF("nodes", 0, host_nodes),
};

static void add_host(JSContext *ctx)
{
    JSValue global = JS_GetGlobalObject(ctx);
    JSValue host = JS_NewObject(ctx);
    JS_SetPropertyFunctionList(ctx, host, HOST_FUNCS, sizeof(HOST_FUNCS) / sizeof(HOST_FUNCS[0]));
    JS_SetPropertyStr(ctx, global, "host", host);
    JS_FreeValue(ctx, global);
}

// ---- Running the script ----

static char *read_file(const char *path, size_t *len)
{
    FILE *f = fopen(path, "rb");
    if (!f) {
        return nullptr;
    }
    fseek(f, 0, SEEK_END);
    *len = static_cast<size_t>(ftell(f));
    fseek(f, 0, SEEK_SET);
    char *buf = static_cast<char *>(malloc(*len + 1));
    if (buf && fread(buf, 1, *len, f) != *len) {
        free(buf);
        buf = nullptr;
    }
    if (buf) {
        buf[*len] = 0;
    }
    fclose(f);
    return buf;
}

static void run_steps(JSContext *ctx, JSModuleDef *mod, lv_display_t *disp)
{
    JSValue ns = JS_GetModuleNamespace(ctx, mod);
    JSValue steps = JS_GetPropertyStr(ctx, ns, "steps");
    int64_t n = 0;
    JS_GetLength(ctx, steps, &n);
    for (int64_t i = 0; i < n; i++) {
        JSValue fn = JS_GetPropertyInt64(ctx, steps, i);
        JSValue name = JS_GetPropertyStr(ctx, fn, "name");
        const char *str = JS_ToCString(ctx, name);
        JSValue r = JS_Call(ctx, fn, JS_UNDEFINED, 0, nullptr);
        if (JS_IsException(r)) {
            fprintf(stderr, "FAIL %s: ", str);
            js_std_dump_error(ctx);
            failures++;
        } else {
            printf("ok   %s\n", str);
        }
        JS_FreeCString(ctx, str);
        JS_FreeValue(ctx, r);
        JS_FreeValue(ctx, name);
        JS_FreeValue(ctx, fn);
        run_loop(test_loop);
        lv_refr_now(disp); // draws whatever the step left on screen
        CHECK(lock_depth == 0);
    }
    if (n == 0) {
        fprintf(stderr, "FAIL: the script exports no steps\n");
        failures++;
    }
    JS_FreeValue(ctx, steps);
    JS_FreeValue(ctx, ns);
}

int main(int argc, char **argv)
{
    if (argc < 2) {
        fprintf(stderr, "usage: %s script.js\n", argv[0]);
        return 2;
    }
    size_t src_len = 0;
    char *src = read_file(argv[1], &src_len);
    if (!src) {
        perror(argv[1]);
        return 2;
    }

    lv_init();
    static uint8_t buf[428 * 40 * 2];
    lv_display_t *disp = lv_display_create(428, 142);
    lv_display_set_buffers(disp, buf, nullptr, sizeof(buf), LV_DISPLAY_RENDER_MODE_PARTIAL);
    lv_display_set_flush_cb(disp, flush_cb);
    test_root = lv_obj_create(lv_screen_active()); // the host's area for the script
    lv_obj_set_size(test_root, LV_PCT(100), LV_PCT(100));

    JSRuntime *rt = JS_NewRuntime();
    JSContext *ctx = JS_NewContext(rt);
    js_std_add_helpers(ctx, 0, nullptr);
    add_host(ctx);
    qjs_esp_loop loop_storage = {};
    test_loop = &loop_storage;
    test_loop->ctx = ctx;

    const lvgl_qjs::config cfg = {.lock = test_lock, .unlock = test_unlock, .root = test_root};
    CHECK(lvgl_qjs::attach(ctx, test_loop, cfg) == ESP_OK);
    CHECK(lvgl_qjs::attach(ctx, test_loop, cfg) == ESP_ERR_INVALID_STATE);

    // Compile, then evaluate, keeping the module for its `steps` export
    JSValue fn = JS_Eval(ctx, src, src_len, argv[1], JS_EVAL_TYPE_MODULE | JS_EVAL_FLAG_COMPILE_ONLY);
    free(src);
    JSModuleDef *mod = JS_IsException(fn) ? nullptr : static_cast<JSModuleDef *>(JS_VALUE_GET_PTR(fn));
    JSValue r = mod ? js_std_await(ctx, JS_EvalFunction(ctx, fn)) : JS_EXCEPTION;
    if (JS_IsException(r)) {
        js_std_dump_error(ctx);
        return 1;
    }
    JS_FreeValue(ctx, r);
    run_loop(test_loop);
    lv_refr_now(disp);
    run_steps(ctx, mod, disp);
    CHECK(!test_loop->failed);

    // Teardown, as a host does it
    lvgl_qjs::detach(ctx);
    CHECK(node_count() == 0);
    run_loop(test_loop); // what qjs_esp_loop_free() would still run
    JS_FreeContext(ctx);
    JS_FreeRuntime(rt); // asserts that no JS object leaked
    CHECK(lock_depth == 0);

    // A second runtime can attach after the first is gone
    lv_obj_clean(test_root);
    rt = JS_NewRuntime();
    ctx = JS_NewContext(rt);
    loop_storage = {};
    test_loop->ctx = ctx;
    CHECK(lvgl_qjs::attach(ctx, test_loop, cfg) == ESP_OK);
    const char *again = "import * as lv from 'lvgl'; lv.create('label', lv.root(), {text: 'again'});";
    r = JS_Eval(ctx, again, strlen(again), "<again>", JS_EVAL_TYPE_MODULE);
    CHECK(!JS_IsException(r));
    JS_FreeValue(ctx, r);
    run_loop(test_loop);
    CHECK(lv_obj_get_child_count(test_root) == 1);
    lvgl_qjs::detach(ctx);
    run_loop(test_loop);
    JS_FreeContext(ctx);
    JS_FreeRuntime(rt);

    lv_obj_delete(test_root);
    lv_display_delete(disp);
    lv_deinit();

    printf(failures ? "FAILED: %d\n" : "all passed\n", failures);
    return failures ? 1 : 0;
}
