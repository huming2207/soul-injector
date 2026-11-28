#include <cstring>
#include <new>

#include "quickjs-libc.h"
#include "lvgl_qjs_priv.hpp"

// The lv.Obj class: object lifetime, the tree, events, and the module itself.
// See node in lvgl_qjs_priv.hpp for who keeps whom alive.

namespace lvgl_qjs::priv
{
    state g = {};

    namespace
    {
        constexpr lv_event_code_t EVENT_CODES[EVENT_KINDS] = {
            LV_EVENT_PRESSED, LV_EVENT_LONG_PRESSED, LV_EVENT_RELEASED, LV_EVENT_CLICKED,
            LV_EVENT_VALUE_CHANGED, LV_EVENT_FOCUSED, LV_EVENT_DEFOCUSED,
        };
        constexpr const char *EVENT_NAMES[EVENT_KINDS] = {
            "onPress", "onLongPress", "onRelease", "onClick", "onChange", "onFocus", "onBlur",
        };

        constexpr uint32_t RETRY_MS = 20;

        enum tree_prop : int16_t { T_PARENT, T_FIRST_CHILD, T_NEXT_SIBLING, T_TYPE, T_CHILD_COUNT, T_DELETED };

        void drain_job(JSContext *ctx, void *arg);
        void dispatch_job(JSContext *ctx, void *arg);

        // ---- Lists ----

        void link(node *n)
        {
            n->prev = nullptr;
            n->next = g.nodes;
            if (g.nodes) {
                g.nodes->prev = n;
            }
            g.nodes = n;
        }

        void unlink(node *n)
        {
            if (n->prev) {
                n->prev->next = n->next;
            } else if (g.nodes == n) {
                g.nodes = n->next;
            }
            if (n->next) {
                n->next->prev = n->prev;
            }
            n->prev = n->next = nullptr;
        }

        // Under the lock, any task. Never waits: if the loop's queue is full,
        // the retry timer posts it later.
        void post_retry_later()
        {
            if (g.retry) {
                lv_timer_resume(g.retry);
            }
        }

        void request_drain()
        {
            if (g.attached && !g.drain_posted) {
                g.drain_posted = qjs_esp_loop_post(g.loop, drain_job, nullptr, 0) == ESP_OK;
                if (!g.drain_posted) {
                    post_retry_later();
                }
            }
        }

        // POSTED is set (by the caller): undone if the queue is full
        void post_dispatch(node *n)
        {
            if (qjs_esp_loop_post(g.loop, dispatch_job, n, 0) != ESP_OK) {
                n->pending.fetch_and(~POSTED);
                post_retry_later();
            }
        }

        // ---- LVGL callbacks: any task, LVGL lock held, no JS ----

        // LV_EVENT_DELETE of an object with a wrapper
        void bind_cb(lv_event_t *e)
        {
            node *n = static_cast<node *>(lv_event_get_user_data(e));
            n->obj = nullptr;
            if (n->held) { // the wrapper's reference is freed on the JS task
                n->held = false;
                n->release = g.release;
                g.release = n;
                request_drain();
            }
        }

        // Every event of an object with handlers. Events of one object that
        // arrive before its dispatch runs are handled together, once each:
        // a slider dragged while the script is busy costs one onChange.
        void input_cb(lv_event_t *e)
        {
            node *n = static_cast<node *>(lv_event_get_user_data(e));
            const lv_event_code_t code = lv_event_get_code(e);
            for (int k = 0; k < EVENT_KINDS; k++) {
                if (EVENT_CODES[k] == code && (n->events & (1u << k))) {
                    if (!(n->pending.fetch_or((1u << k) | POSTED) & POSTED)) {
                        post_dispatch(n);
                    }
                    return;
                }
            }
        }

        // LVGL timer: posts what the loop's queue was too full for. Events
        // that came in meanwhile are already in `fired`.
        void retry_cb(lv_timer_t *t)
        {
            bool waiting = false;
            if (g.attached) {
                if (!g.drain_posted && (g.release || lv_obj_get_child_count(g.trash))) {
                    request_drain();
                    waiting = !g.drain_posted;
                }
                for (node *n = g.nodes; n; n = n->next) {
                    const uint32_t p = n->pending.load();
                    if ((p & FIRED_MASK) && !(p & POSTED) && !(n->pending.fetch_or(POSTED) & POSTED)) {
                        post_dispatch(n);
                        waiting = waiting || !(n->pending.load() & POSTED);
                    }
                }
            }
            if (!waiting) {
                lv_timer_pause(t);
            }
        }

        // ---- JS task ----

        void report_error(JSContext *ctx)
        {
            JSValue e = JS_GetException(ctx);
            if (JS_IsUncatchableError(e)) { // out of memory, or the loop's own interrupt
                qjs_esp_loop_fail(g.loop, e);
                JS_FreeValue(ctx, e);
                return;
            }
            JS_Throw(ctx, e);
            js_std_dump_error(ctx);
        }

        void dispatch_job(JSContext *ctx, void *arg)
        {
            node *n = static_cast<node *>(arg);
            if (!g.runtime_ok) {
                return; // the arena goes, nodes and all
            }
            // Takes the events and clears POSTED in one go: an event from now on
            // posts a new dispatch. No LVGL lock, so no wait for a frame.
            const uint32_t fired = n->pending.exchange(0) & FIRED_MASK;
            bool alive = n->obj.load() != nullptr;
            if (JS_IsUndefined(n->self)) { // finalized while this was queued
                js_free(ctx, n);
                return;
            }
            // A stopping loop (or qjs_esp_loop_free() running what's left)
            // runs no more JS
            if (!alive || !g.attached || qjs_esp_loop_stopping(g.loop)) {
                return;
            }
            JSValue self = JS_DupValue(ctx, n->self);
            for (int k = 0; k < EVENT_KINDS; k++) {
                if (!(fired & (1u << k))) {
                    continue;
                }
                if (k) { // an earlier handler may have deleted it, or failed for good
                    alive = n->obj.load() != nullptr;
                }
                if (!alive || qjs_esp_loop_stopping(g.loop)) {
                    break;
                }
                JSValue fn = JS_GetProperty(ctx, self, g.handler_atom[k]);
                if (JS_IsFunction(ctx, fn)) {
                    JSValue ret = JS_Call(ctx, fn, self, 1, &self);
                    if (JS_IsException(ret)) {
                        report_error(ctx);
                    }
                    JS_FreeValue(ctx, ret);
                }
                JS_FreeValue(ctx, fn);
            }
            JS_FreeValue(ctx, self);
        }

        // Empties trash and frees the references LVGL handed back. Freeing a
        // wrapper can finalize it and send more objects to trash: repeat.
        void drain(JSContext *ctx)
        {
            for (;;) {
                node *list;
                {
                    lv_lock l;
                    if (!l.ok) {
                        return;
                    }
                    g.drain_posted = false;
                    if (g.trash && lv_obj_get_child_count(g.trash)) {
                        lv_obj_clean(g.trash);
                    }
                    list = g.release;
                    g.release = nullptr;
                }
                if (!list) {
                    return;
                }
                while (list) {
                    node *n = list;
                    list = n->release;
                    n->release = nullptr;
                    JS_FreeValue(ctx, n->self); // may free n
                }
            }
        }

        void drain_job(JSContext *ctx, void *)
        {
            if (g.attached) {
                drain(ctx);
            }
        }

        void finalizer(JSRuntime *rt, JSValueConst val)
        {
            node *n = static_cast<node *>(JS_GetOpaque(val, g.class_id));
            if (!n) {
                return;
            }
            lv_lock l(true); // recursive: fine inside a native call that holds it
            n->self = JS_UNDEFINED;
            // Held, yet finalized: collected with the detached object that
            // reported it in gc_mark. QuickJS frees it, our reference with it.
            n->held = false;
            if (n->obj) { // detached, or inside a detached object: it goes with its wrapper
                lv_obj_remove_event_cb_with_user_data(n->obj, bind_cb, n);
                if (n->listening) {
                    lv_obj_remove_event_cb_with_user_data(n->obj, input_cb, n);
                }
                if (lv_obj_get_parent(n->obj) == g.limbo) {
                    lv_obj_set_parent(n->obj, g.trash);
                    request_drain();
                }
                n->obj = nullptr;
            }
            unlink(n);
            if (!(n->pending.load() & POSTED)) { // input_cb is gone: nothing sets it any more
                js_free_rt(rt, n);
            }
        }

        node *find_node(lv_obj_t *obj);

        void mark_held_in(JSRuntime *rt, lv_obj_t *obj, JS_MarkFunc *mark_func)
        {
            const uint32_t count = lv_obj_get_child_count(obj);
            for (uint32_t i = 0; i < count; i++) {
                lv_obj_t *c = lv_obj_get_child(obj, i);
                node *n = find_node(c);
                if (n && n->held) {
                    JS_MarkValue(rt, n->self, mark_func);
                }
                mark_held_in(rt, c, mark_func);
            }
        }

        // The only JS value a node keeps is its own wrapper, held while the
        // object is in the screen tree (a root on purpose) or inside a
        // detached object. The detached object's wrapper reports those, so
        // the GC sees them as its children.
        void gc_mark(JSRuntime *rt, JSValueConst val, JS_MarkFunc *mark_func)
        {
            node *n = static_cast<node *>(JS_GetOpaque(val, g.class_id));
            if (!n || !n->obj) {
                return;
            }
            lv_lock l(true);
            if (n->obj && lv_obj_get_parent(n->obj) == g.limbo) {
                mark_held_in(rt, n->obj, mark_func);
            }
        }

        const JSClassDef OBJ_CLASS = {
            .class_name = "Obj",
            .finalizer = finalizer,
            .gc_mark = gc_mark,
            .call = nullptr,
            .exotic = nullptr,
        };

        node *find_node(lv_obj_t *obj)
        {
            const uint32_t count = lv_obj_get_event_count(obj);
            for (uint32_t i = 0; i < count; i++) {
                lv_event_dsc_t *dsc = lv_obj_get_event_dsc(obj, i);
                if (lv_event_dsc_get_cb(dsc) == bind_cb) {
                    return static_cast<node *>(lv_event_dsc_get_user_data(dsc));
                }
            }
            return nullptr;
        }

        bool detached(const lv_obj_t *parent)
        {
            return parent == g.limbo || parent == g.trash;
        }

        // Under the lock: the object went into the tree, the tree keeps the wrapper
        void hold(JSContext *ctx, node *n)
        {
            if (!n->held) {
                n->held = true;
                JS_DupValue(ctx, n->self);
            }
        }

        // The caller still has the wrapper, so this never frees it
        void unhold(JSContext *ctx, node *n)
        {
            if (n->held) {
                n->held = false;
                JS_FreeValue(ctx, n->self);
            }
        }

        // Screens and the root are the host's
        JSValue check_owned(JSContext *ctx, lv_obj_t *o, const char *what)
        {
            if (!lv_obj_get_parent(o) || o == g.cfg.root) {
                return JS_ThrowTypeError(ctx, "%s: the root and screens can't be %s", what, what);
            }
            return JS_UNDEFINED;
        }

        bool is_nullish(JSValueConst v)
        {
            return JS_IsUndefined(v) || JS_IsNull(v);
        }

        // ---- lv.Obj methods ----

        // parent.insert(child, anchor = null): moves child into parent, before
        // anchor or at the end. As insertBefore() in the DOM.
        JSValue js_insert(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
        {
            lv_lock l;
            if (!l.ok) {
                return lock_error(ctx);
            }
            lv_obj_t *p = obj_of(ctx, this_val);
            lv_obj_t *c = p ? obj_of(ctx, argv[0]) : nullptr;
            if (!c) {
                return JS_EXCEPTION;
            }
            lv_obj_t *a = nullptr;
            if (argc > 1 && !is_nullish(argv[1])) {
                a = obj_of(ctx, argv[1]);
                if (!a) {
                    return JS_EXCEPTION;
                }
                if (lv_obj_get_parent(a) != p) {
                    return JS_ThrowRangeError(ctx, "insert: the anchor isn't a child of this object");
                }
            }
            for (lv_obj_t *x = p; x; x = lv_obj_get_parent(x)) {
                if (x == c) {
                    return JS_ThrowRangeError(ctx, "insert: an object can't go inside itself");
                }
            }
            if (JS_IsException(check_owned(ctx, c, "moved"))) {
                return JS_EXCEPTION;
            }

            if (lv_obj_get_parent(c) != p) {
                lv_obj_set_parent(c, p); // now the last child
            }
            if (!a) {
                lv_obj_move_to_index(c, -1);
            } else if (a != c) {
                const int32_t ia = lv_obj_get_index(a);
                const int32_t ic = lv_obj_get_index(c);
                lv_obj_move_to_index(c, ic < ia ? ia - 1 : ia);
            }
            hold(ctx, static_cast<node *>(JS_GetOpaque(argv[0], g.class_id)));
            return JS_UNDEFINED;
        }

        // obj.remove(): out of the tree, but alive while the script keeps it
        JSValue js_remove(JSContext *ctx, JSValueConst this_val, int, JSValueConst *)
        {
            lv_lock l;
            if (!l.ok) {
                return lock_error(ctx);
            }
            lv_obj_t *o = obj_of(ctx, this_val);
            if (!o || JS_IsException(check_owned(ctx, o, "removed"))) {
                return JS_EXCEPTION;
            }
            if (lv_obj_get_parent(o) != g.limbo) {
                lv_obj_set_parent(o, g.limbo);
            }
            unhold(ctx, static_cast<node *>(JS_GetOpaque(this_val, g.class_id)));
            return JS_UNDEFINED;
        }

        // obj.delete(): gone now, children too. Once more is a no-op.
        JSValue js_delete(JSContext *ctx, JSValueConst this_val, int, JSValueConst *)
        {
            node *n = static_cast<node *>(JS_GetOpaque2(ctx, this_val, g.class_id));
            if (!n) {
                return JS_EXCEPTION;
            }
            lv_lock l;
            if (!l.ok) {
                return lock_error(ctx);
            }
            if (!n->obj) {
                return JS_UNDEFINED;
            }
            if (JS_IsException(check_owned(ctx, n->obj, "deleted"))) {
                return JS_EXCEPTION;
            }
            lv_obj_delete(n->obj); // bind_cb clears n->obj
            return JS_UNDEFINED;
        }

        JSValue get_tree(JSContext *ctx, JSValueConst this_val, int magic)
        {
            node *n = static_cast<node *>(JS_GetOpaque2(ctx, this_val, g.class_id));
            if (!n) {
                return JS_EXCEPTION;
            }
            lv_lock l;
            if (!l.ok) {
                return lock_error(ctx);
            }
            if (magic == T_DELETED) {
                return JS_NewBool(ctx, !n->obj);
            }
            lv_obj_t *o = obj_of(ctx, this_val);
            if (!o) {
                return JS_EXCEPTION;
            }
            lv_obj_t *p = lv_obj_get_parent(o);
            lv_obj_t *r = nullptr;
            switch (magic) {
            case T_PARENT:
                r = detached(p) ? nullptr : p;
                break;
            case T_FIRST_CHILD:
                r = lv_obj_get_child(o, 0);
                break;
            case T_NEXT_SIBLING:
                r = detached(p) ? nullptr : lv_obj_get_sibling(o, 1);
                break;
            case T_TYPE:
                return JS_NewString(ctx, type_name(o));
            case T_CHILD_COUNT:
                return JS_NewUint32(ctx, lv_obj_get_child_count(o));
            default:
                break;
            }
            return r ? wrap(ctx, r) : JS_NULL;
        }

        JSValue get_handler(JSContext *ctx, JSValueConst this_val, int magic)
        {
            if (!JS_GetOpaque2(ctx, this_val, g.class_id)) {
                return JS_EXCEPTION;
            }
            return JS_GetProperty(ctx, this_val, g.handler_atom[magic]);
        }

        // obj.onClick = fn, or null to stop
        JSValue set_handler(JSContext *ctx, JSValueConst this_val, JSValueConst val, int magic)
        {
            node *n = static_cast<node *>(JS_GetOpaque2(ctx, this_val, g.class_id));
            if (!n) {
                return JS_EXCEPTION;
            }
            const bool on = JS_IsFunction(ctx, val);
            if (!on && !is_nullish(val)) {
                return JS_ThrowTypeError(ctx, "%s: expected a function or null", EVENT_NAMES[magic]);
            }
            {
                lv_lock l;
                if (!l.ok) {
                    return lock_error(ctx);
                }
                if (!n->obj) { // dropping a handler of a deleted object is fine
                    return on ? JS_ThrowReferenceError(ctx, "this LVGL object was deleted") : JS_UNDEFINED;
                }
                if (on && !n->listening) {
                    if (!lv_obj_add_event_cb(n->obj, input_cb, LV_EVENT_ALL, n)) {
                        return JS_ThrowOutOfMemory(ctx);
                    }
                    n->listening = true;
                }
            }
            // Stored once LVGL will call it. A failed store leaves the bit
            // clear: input_cb ignores events without one.
            const int r = on ? JS_SetProperty(ctx, this_val, g.handler_atom[magic], JS_DupValue(ctx, val))
                             : JS_DeleteProperty(ctx, this_val, g.handler_atom[magic], 0);

            lv_lock l(true);
            if (r >= 0) {
                n->events = on ? n->events | (1u << magic) : n->events & ~(1u << magic);
            }
            if (!n->events && n->listening && n->obj) {
                lv_obj_remove_event_cb_with_user_data(n->obj, input_cb, n);
                n->listening = false;
            }
            return r < 0 ? JS_EXCEPTION : JS_UNDEFINED;
        }

#define HANDLER(k) JS_CGETSET_MAGIC_DEF(EVENT_NAMES[k], get_handler, set_handler, k)
        const JSCFunctionListEntry OBJ_FUNCS[] = {
            JS_CFUNC_DEF("insert", 2, js_insert),
            JS_CFUNC_DEF("remove", 0, js_remove),
            JS_CFUNC_DEF("delete", 0, js_delete),
            JS_CFUNC_DEF("setStyle", 3, js_set_style),
            JS_CGETSET_MAGIC_DEF("parent", get_tree, nullptr, T_PARENT),
            JS_CGETSET_MAGIC_DEF("firstChild", get_tree, nullptr, T_FIRST_CHILD),
            JS_CGETSET_MAGIC_DEF("nextSibling", get_tree, nullptr, T_NEXT_SIBLING),
            JS_CGETSET_MAGIC_DEF("type", get_tree, nullptr, T_TYPE),
            JS_CGETSET_MAGIC_DEF("childCount", get_tree, nullptr, T_CHILD_COUNT),
            JS_CGETSET_MAGIC_DEF("deleted", get_tree, nullptr, T_DELETED),
            HANDLER(EV_PRESS),
            HANDLER(EV_LONG_PRESS),
            HANDLER(EV_RELEASE),
            HANDLER(EV_CLICK),
            HANDLER(EV_CHANGE),
            HANDLER(EV_FOCUS),
            HANDLER(EV_BLUR),
            JS_PROP_STRING_DEF("[Symbol.toStringTag]", "lv.Obj", JS_PROP_CONFIGURABLE),
        };
#undef HANDLER

        // ---- Module functions ----

        // Copies props' own enumerable properties onto target, through the
        // accessors, as Object.assign() does. -1 with an exception pending.
        int assign(JSContext *ctx, JSValueConst target, JSValueConst props)
        {
            JSPropertyEnum *tab = nullptr;
            uint32_t count = 0;
            if (JS_GetOwnPropertyNames(ctx, &tab, &count, props, JS_GPN_STRING_MASK | JS_GPN_ENUM_ONLY)) {
                return -1;
            }
            int ret = 0;
            for (uint32_t i = 0; i < count && ret == 0; i++) {
                JSValue v = JS_GetProperty(ctx, props, tab[i].atom);
                ret = JS_IsException(v) || JS_SetProperty(ctx, target, tab[i].atom, v) < 0 ? -1 : 0;
            }
            JS_FreePropertyEnum(ctx, tab, count);
            return ret;
        }

        // lv.create(type, parent = null, props = {}). Without a parent the
        // object starts detached, for insert() later.
        JSValue js_create(JSContext *ctx, JSValueConst, int argc, JSValueConst *argv)
        {
            const char *type = JS_ToCString(ctx, argv[0]);
            if (!type) {
                return JS_EXCEPTION;
            }
            JSValue v = JS_EXCEPTION;
            {
                lv_lock l;
                lv_obj_t *parent = g.limbo;
                if (!l.ok) {
                    lock_error(ctx);
                } else if (argc > 1 && !is_nullish(argv[1])) {
                    parent = obj_of(ctx, argv[1]);
                }
                lv_obj_t *o = l.ok && parent ? create_widget(type, parent) : nullptr;
                if (o) {
                    v = wrap(ctx, o);
                    if (JS_IsException(v)) {
                        lv_obj_delete(o);
                    }
                } else if (l.ok && parent) {
                    JS_ThrowTypeError(ctx, "create: no widget type '%s'", type);
                }
            }
            JS_FreeCString(ctx, type);
            if (JS_IsException(v) || argc < 3 || !JS_IsObject(argv[2]) || assign(ctx, v, argv[2]) == 0) {
                return v;
            }
            js_delete(ctx, v, 0, nullptr); // half made: don't leave it on screen
            JS_FreeValue(ctx, v);
            return JS_EXCEPTION;
        }

        // lv.root(): where the script's UI goes
        JSValue js_root(JSContext *ctx, JSValueConst, int, JSValueConst *)
        {
            lv_lock l;
            if (!l.ok) {
                return lock_error(ctx);
            }
            return wrap(ctx, g.cfg.root ? g.cfg.root : lv_screen_active());
        }

        // lv.pct(50): 50 % of the parent, for width, height, x, y
        JSValue js_pct(JSContext *ctx, JSValueConst, int, JSValueConst *argv)
        {
            int32_t v;
            if (JS_ToInt32(ctx, &v, argv[0])) {
                return JS_EXCEPTION;
            }
            return JS_NewInt32(ctx, LV_PCT(v));
        }

        const JSCFunctionListEntry MODULE_FUNCS[] = {
            JS_CFUNC_DEF("create", 3, js_create),
            JS_CFUNC_DEF("root", 0, js_root),
            JS_CFUNC_DEF("pct", 1, js_pct),
        };
        constexpr size_t MODULE_FUNC_COUNT = sizeof(MODULE_FUNCS) / sizeof(MODULE_FUNCS[0]);

        int module_init(JSContext *ctx, JSModuleDef *m)
        {
            if (JS_SetModuleExportList(ctx, m, MODULE_FUNCS, MODULE_FUNC_COUNT) < 0) {
                return -1;
            }
            return JS_SetModuleExportList(ctx, m, const_list, const_count);
        }

        bool init_class(JSContext *ctx)
        {
            JSRuntime *rt = JS_GetRuntime(ctx);
            JS_NewClassID(rt, &g.class_id);
            if (!JS_IsRegisteredClass(rt, g.class_id) && JS_NewClass(rt, g.class_id, &OBJ_CLASS) < 0) {
                return false;
            }
            JSValue proto = JS_NewObject(ctx);
            if (JS_IsException(proto) ||
                JS_SetPropertyFunctionList(ctx, proto, OBJ_FUNCS, sizeof(OBJ_FUNCS) / sizeof(OBJ_FUNCS[0])) < 0 ||
                !init_props(ctx, proto)) {
                JS_FreeValue(ctx, proto);
                return false;
            }
            JS_SetClassProto(ctx, g.class_id, proto);
            return true;
        }

        bool init_handler_atoms(JSContext *ctx)
        {
            for (int k = 0; k < EVENT_KINDS; k++) {
                JSValue sym = JS_NewSymbol(ctx, EVENT_NAMES[k], false);
                if (JS_IsException(sym)) {
                    return false;
                }
                g.handler_atom[k] = JS_ValueToAtom(ctx, sym);
                JS_FreeValue(ctx, sym);
            }
            return true;
        }

        void free_handler_atoms(JSContext *ctx)
        {
            for (JSAtom &a : g.handler_atom) {
                if (a != JS_ATOM_NULL) {
                    JS_FreeAtom(ctx, a);
                    a = JS_ATOM_NULL;
                }
            }
        }
    } // namespace

    JSValue lock_error(JSContext *ctx)
    {
        return JS_ThrowInternalError(ctx, "LVGL is busy");
    }

    lv_obj_t *obj_of(JSContext *ctx, JSValueConst v)
    {
        node *n = static_cast<node *>(JS_GetOpaque2(ctx, v, g.class_id));
        if (!n) {
            return nullptr;
        }
        if (!n->obj) {
            JS_ThrowReferenceError(ctx, "this LVGL object was deleted");
            return nullptr;
        }
        return n->obj;
    }

    JSValue wrap(JSContext *ctx, lv_obj_t *obj)
    {
        node *n = find_node(obj);
        if (n) {
            return JS_DupValue(ctx, n->self);
        }
        // A finalizer may run in these two allocations; it never deletes an
        // object, so obj stays valid.
        void *mem = js_malloc(ctx, sizeof(node));
        if (!mem) {
            return JS_EXCEPTION;
        }
        n = new (mem) node{}; // trivially destructible: js_free() ends it
        JSValue v = JS_NewObjectClass(ctx, g.class_id);
        if (JS_IsException(v)) {
            js_free(ctx, n);
            return v;
        }
        if (!lv_obj_add_event_cb(obj, bind_cb, LV_EVENT_DELETE, n)) {
            js_free(ctx, n);
            JS_FreeValue(ctx, v);
            return JS_ThrowOutOfMemory(ctx);
        }
        n->obj = obj;
        n->self = v;
        link(n);
        JS_SetOpaque(v, n);
        if (!detached(lv_obj_get_parent(obj))) {
            hold(ctx, n);
        }
        return v;
    }
} // namespace lvgl_qjs::priv

namespace lvgl_qjs
{
    using namespace priv;

    esp_err_t attach(JSContext *ctx, qjs_esp_loop_t *loop, const config &cfg)
    {
        if (g.attached) {
            return ESP_ERR_INVALID_STATE;
        }
        if (!ctx || !loop || !cfg.lock || !cfg.unlock) {
            return ESP_ERR_INVALID_ARG;
        }
        const JSClassID class_id = g.class_id; // QuickJS hands out class ids once per process
        g = {};
        g.class_id = class_id;
        g.ctx = ctx;
        g.loop = loop;
        g.cfg = cfg;
        g.runtime_ok = true;

        if (!init_class(ctx) || !init_handler_atoms(ctx)) {
            free_handler_atoms(ctx);
            return ESP_ERR_NO_MEM;
        }
        JSModuleDef *m = JS_NewCModule(ctx, "lvgl", module_init);
        if (!m || JS_AddModuleExportList(ctx, m, MODULE_FUNCS, MODULE_FUNC_COUNT) < 0 ||
            JS_AddModuleExportList(ctx, m, const_list, const_count) < 0) {
            free_handler_atoms(ctx);
            return ESP_ERR_NO_MEM;
        }

        lv_lock l;
        if (!l.ok) {
            free_handler_atoms(ctx);
            return ESP_ERR_TIMEOUT;
        }
        g.limbo = lv_obj_create(nullptr);
        g.trash = lv_obj_create(nullptr);
        g.retry = lv_timer_create(retry_cb, RETRY_MS, nullptr);
        if (!g.limbo || !g.trash || !g.retry) {
            if (g.limbo) {
                lv_obj_delete(g.limbo);
            }
            if (g.trash) {
                lv_obj_delete(g.trash);
            }
            if (g.retry) {
                lv_timer_delete(g.retry);
            }
            g.limbo = g.trash = nullptr;
            g.retry = nullptr;
            free_handler_atoms(ctx);
            return ESP_ERR_NO_MEM;
        }
        lv_timer_pause(g.retry);
        g.attached = true;
        return ESP_OK;
    }

    void detach(JSContext *ctx, bool runtime_ok)
    {
        if (!g.attached) {
            return;
        }
        {
            lv_lock l(true);
            g.runtime_ok = runtime_ok;
            g.attached = false; // queued dispatches and drains do nothing now
            lv_timer_delete(g.retry);
            g.retry = nullptr;
            for (node *n = g.nodes, *next; n; n = next) {
                next = n->next;
                if (n->obj) {
                    lv_obj_remove_event_cb_with_user_data(n->obj, bind_cb, n);
                    if (n->listening) {
                        lv_obj_remove_event_cb_with_user_data(n->obj, input_cb, n);
                    }
                    n->obj = nullptr;
                }
                if (n->held) {
                    n->held = false;
                    n->release = g.release;
                    g.release = n;
                }
                n->prev = n->next = nullptr;
            }
            g.nodes = nullptr;
            lv_obj_delete(g.limbo); // no binding callbacks left on anything in them
            lv_obj_delete(g.trash);
            g.limbo = g.trash = nullptr;
        }
        node *list = g.release;
        g.release = nullptr;
        if (!runtime_ok) {
            return;
        }
        while (list) {
            node *n = list;
            list = n->release;
            n->release = nullptr;
            JS_FreeValue(ctx, n->self); // the finalizer frees n, now or with the context
        }
        free_handler_atoms(ctx);
    }
} // namespace lvgl_qjs
