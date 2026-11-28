#pragma once

#include <atomic>

#include "lvgl_qjs.hpp"

namespace lvgl_qjs::priv
{
    // Event handlers, one bit each in node::events and node::pending.
    enum event_kind : uint8_t { EV_PRESS, EV_LONG_PRESS, EV_RELEASE, EV_CLICK, EV_CHANGE, EV_FOCUS, EV_BLUR, EVENT_KINDS };

    // What a JS wrapper knows of its LVGL object. One per wrapper, made with
    // it and freed with it (or by the last pending dispatch, see POSTED).
    //
    // Who keeps whom alive:
    //  - An object in the screen tree keeps its wrapper: `held`, a counted
    //    reference to `self`. So a wrapper and the properties a script put on
    //    it (handlers among them) live as long as the object does.
    //  - A detached object (made without a parent, or remove()d) sits on a
    //    hidden screen, "limbo", and lives as long as its wrapper: when the
    //    script drops the wrapper, the finalizer moves the object to a second
    //    hidden screen, "trash", which the next drain empties. (A finalizer
    //    can run inside any allocation, so it never deletes objects itself.)
    //  - Wrappers inside a detached object stay held, and the detached
    //    object's wrapper reports them to the GC (gc_mark), so a cycle through
    //    them (a button's onClick closing over its detached container) is
    //    still collected, all of it at once.
    // LVGL telling us an object was deleted clears `obj`; the wrapper then
    // throws on use.
    // node::pending: the events seen and not yet dispatched (one bit per
    // event_kind), and POSTED: a dispatch for this node is queued; it frees
    // the node if the wrapper is gone by then. One word, so dispatch_job takes
    // both at once without the LVGL lock (the LVGL task holds that for whole
    // frames).
    constexpr uint32_t POSTED = 1u << 31;
    constexpr uint32_t FIRED_MASK = (1u << EVENT_KINDS) - 1;
    static_assert(std::atomic<uint32_t>::is_always_lock_free && std::atomic<lv_obj_t *>::is_always_lock_free,
                  "LVGL callbacks and dispatch_job share these without a lock");

    struct node {
        std::atomic<lv_obj_t *> obj; // null once deleted; written under the LVGL lock, dispatch_job reads it without
        JSValue self;   // the wrapper; counted only while held, undefined once finalized
        node *prev;     // every node, for detach()
        node *next;
        node *release;  // release list: held was dropped by LVGL, self still to free
        std::atomic<uint32_t> pending; // fired events | POSTED
        uint16_t events; // handlers set
        bool held;
        bool listening; // input_cb is registered
    };

    struct state {
        bool attached;
        bool runtime_ok;
        bool drain_posted;
        JSContext *ctx;
        qjs_esp_loop_t *loop;
        config cfg;
        JSClassID class_id;
        lv_obj_t *limbo;
        lv_obj_t *trash;
        node *nodes;          // all of them
        node *release;        // LVGL deleted these while held
        lv_timer_t *retry;    // posts what a full loop queue refused; paused while nothing is
        JSAtom handler_atom[EVENT_KINDS]; // symbols the handlers are stored under, on the wrapper
    };

    extern state g;

    // The LVGL lock, for one native call. config::lock(0) waits for as long
    // as it takes, so ok is false only with a broken lock. must: for the
    // paths that can't fail (finalizer, GC, detach): try until it works.
    class lv_lock
    {
    public:
        explicit lv_lock(bool must = false) : ok(take(must)) {}
        ~lv_lock()
        {
            if (ok) {
                g.cfg.unlock();
            }
        }
        lv_lock(const lv_lock &) = delete;
        lv_lock &operator=(const lv_lock &) = delete;

        const bool ok;

    private:
        static bool take(bool must)
        {
            bool got = g.cfg.lock(0);
            while (must && !got) {
                got = g.cfg.lock(0);
            }
            return got;
        }
    };

    // Under the lock. The LVGL object behind a wrapper, or nullptr with a JS
    // exception pending (not a wrapper, or the object was deleted).
    lv_obj_t *obj_of(JSContext *ctx, JSValueConst v);
    JSValue lock_error(JSContext *ctx);

    // Under the lock. The wrapper of obj, made if it has none yet.
    JSValue wrap(JSContext *ctx, lv_obj_t *obj);

    // Properties of lv.Obj (props.cpp)
    bool init_props(JSContext *ctx, JSValueConst proto);
    const char *type_name(const lv_obj_t *obj);
    lv_obj_t *create_widget(const char *type, lv_obj_t *parent);
    // obj.setStyle(name, value, selector)
    JSValue js_set_style(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv);
    // Module exports: constants
    extern const JSCFunctionListEntry *const const_list;
    extern const size_t const_count;
} // namespace lvgl_qjs::priv
