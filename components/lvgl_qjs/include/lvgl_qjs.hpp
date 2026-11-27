#pragma once

#include <cstdint>

#include <esp_err.h>
#include <lvgl.h>

#include "qjs_esp.h"
#include "quickjs.h"

// LVGL for QuickJS: the "lvgl" module.
//
//   import * as lv from "lvgl";
//   const b = lv.create("button", lv.root(), { width: 120, bgColor: 0x2d5a87 });
//   lv.create("label", b, { text: "Start", align: lv.ALIGN.CENTER });
//   b.onClick = () => start();
//
// Every LVGL object a script sees is one JS object (an lv.Obj): properties for
// what the widget shows (text, value, checked...) and for its local styles
// (width, bgColor, padRow...), event handlers as onClick/onChange..., and
// DOM-like tree calls (insert, remove, parent, firstChild, nextSibling) that a
// SolidJS universal renderer maps onto one to one. See README.md.
//
// One context at a time. Everything here runs on the JS task, except that the
// LVGL task (any task that holds the LVGL lock) may run the binding's event
// callbacks; those only post to the JS loop and never wait for it.
namespace lvgl_qjs
{
    struct config {
        // The LVGL lock, held around every LVGL call from JS. Must be recursive
        // (lvgl_port_lock is). timeout_ms 0: wait for as long as it takes.
        bool (*lock)(uint32_t timeout_ms);
        void (*unlock)();
        // What lv.root() returns: the part of the screen the script owns.
        // nullptr: the active screen at the time of the call.
        lv_obj_t *root;
    };

    // JS task, before the script is evaluated. Registers the "lvgl" module in
    // ctx. Events reach handlers through loop, so a UI script needs the loop
    // kept alive (qjs_esp_loop_keep_alive) to keep handling them.
    esp_err_t attach(JSContext *ctx, qjs_esp_loop_t *loop, const config &cfg);

    // JS task, before qjs_esp_loop_free(). Objects the script left detached
    // are deleted; objects in the screen tree stay where they are (clean the
    // root to remove them) but stop calling the script.
    // runtime_ok false: the runtime will be dropped with qjs_esp_arena_reset()
    // rather than freed, so no JS value is touched.
    void detach(JSContext *ctx, bool runtime_ok = true);
} // namespace lvgl_qjs
