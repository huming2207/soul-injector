# lvgl_qjs: LVGL for QuickJS

The `lvgl` module for QuickJS-NG scripts: build and drive an LVGL screen from
JavaScript, by hand or with SolidJS on top.

```js
import * as lv from "lvgl";

const col = lv.create("obj", lv.root(), { width: lv.pct(100), height: lv.pct(100),
                                          flexFlow: lv.FLEX_FLOW.COLUMN, gap: 6 });
const title = lv.create("label", col, { text: "Ready", textFont: lv.FONT.MONTSERRAT_28 });
const go = lv.create("button", col, { width: 120, bgColor: 0x2d5a87 });
lv.create("label", go, { text: "Start", align: lv.ALIGN.CENTER });
go.onClick = () => { title.text = "Running"; };
```

## Why not a 1:1 binding of the C API

LVGL's names are kept; its function-per-property C API is not.

* **RAM.** QuickJS makes a function object for every exported C function
  when the module loads. LVGL 9.6's headers declare about 2,600 public
  functions: 100 KB or more of heap per context before a script does anything.
  Here, each property is one getter/setter pair on a shared prototype, built
  from tables in flash.
* **Safety.** A 1:1 binding hands scripts raw `lv_obj_t *`. A deleted object
  (a parent cleaned from C, a screen change) then crashes the firmware that is
  also programming a board. Here, every object is a wrapper that learns about
  the deletion (`LV_EVENT_DELETE`) and throws a `ReferenceError` instead.
* **Threads.** Every LVGL call must hold the LVGL lock, and events arrive on the
  LVGL task, not the JS task. The binding takes the lock itself and posts
  events to the JS loop. A 1:1 binding leaves both jobs to every script.
* **SolidJS** (or any renderer) needs a DOM-like node API: create, insert
  before an anchor, remove, parent, first child, next sibling, set a property
  by name. A 1:1 binding would need that layer written in JS on top, which is
  slower and allocates more than doing it natively.

What stays 1:1 is the vocabulary, so LVGL's docs still apply. Widget types are
LVGL's (`"button"`, `"slider"`). Style properties are `LV_STYLE_*` in
camelCase (`LV_STYLE_BG_COLOR` → `bgColor`, `LV_STYLE_PAD_ROW` → `padRow`).
Constants keep their names (`LV_ALIGN_CENTER` → `lv.ALIGN.CENTER`,
`LV_PART_INDICATOR` → `lv.PART.INDICATOR`). Styles are local styles with
parts and states, as `lv_obj_set_style_*()` sets them.

## API

Types: `js/lvgl.d.ts`.

| | |
|---|---|
| `lv.create(type, parent?, props?)` | `"obj" "label" "button" "bar" "slider" "switch" "checkbox" "arc" "spinner" "textarea"`. Without a parent the object starts detached. |
| `lv.root()` | The part of the screen the script owns (set by the host). |
| `lv.pct(n)`, `lv.SIZE_CONTENT` | Sizes and positions. |
| `obj.text` | label, checkbox, textarea. Setting the same text is free (no redraw). |
| `obj.value`, `.min`, `.max` | bar, slider, arc. |
| `obj.checked`, `.disabled`, `.hidden`, `.clickable`, `.scrollable`, `.checkable` | States and flags. |
| `obj.width`, `.bgColor`, `.padRow` ... | Local style of the main part, default state. Colours are `0xRRGGBB`, fonts `lv.FONT.*`. `pad` sets all four sides, `gap` both row and column, `flexFlow` also turns the flex layout on. Setting a value the object already has is free. |
| `obj.setStyle(name, value, selector)` | Any part and state: `btn.setStyle("bgColor", 0x335577, lv.STATE.PRESSED)`. |
| `obj.onClick`, `onPress`, `onLongPress`, `onRelease`, `onChange`, `onFocus`, `onBlur` | A function `(target) => {}` (`this` is the object too), or `null`. |
| `obj.insert(child, anchor?)` | Moves `child` here, before `anchor` or at the end (DOM `insertBefore`). |
| `obj.remove()` | Out of the tree, still usable; `insert()` puts it back. |
| `obj.delete()` | Gone, with its children. |
| `obj.parent`, `.firstChild`, `.nextSibling`, `.childCount`, `.type`, `.deleted` | The tree. The same LVGL object always gives the same JS object. |

Wrong types throw `TypeError` (`button.text = ...`), bad values `RangeError`,
and a deleted object throws `ReferenceError`. The root and screens can't be
removed, deleted or moved.

## Lifetime

* An object in the screen tree keeps its JS wrapper alive, and with it the
  handlers and any properties the script put on it. A script doesn't need to
  hold on to what it put on screen.
* A detached object (created without a parent, or `remove()`d) sits on a
  hidden screen and lives as long as the script references it. When the
  wrapper is collected, the object is deleted. This is how Solid's removed
  nodes go away.
* Deleting from C (`lv_obj_clean(root)`, a screen with auto-delete) is fine:
  the wrappers find out and throw on use.

Cost per object: the LVGL object, one QuickJS object, a 40-byte node, and
one LVGL event descriptor (two with handlers). Property updates allocate
nothing in the binding.

## Threads and events

Every native call takes the LVGL lock (recursive: `lvgl_port_lock`) for the
call only, never across JS code. Event callbacks run on the LVGL task and
never wait for JS: they set a bit on the object and post one dispatch to the
JS loop. Events of one object that arrive before the script gets to them are
delivered once each, so dragging a slider while the script is busy costs one
`onChange` with the latest value. If the loop's queue is full the events
wait for the next one.

## Hosting

```cpp
#include "lvgl_qjs.hpp"

// JS task, before the script runs
const lvgl_qjs::config cfg = {.lock = lvgl_port_lock, .unlock = lvgl_port_unlock, .root = my_area};
lvgl_qjs::attach(ctx, loop, cfg);
qjs_esp_loop_keep_alive(loop, true); // a UI waits for events
// ... evaluate the script, qjs_esp_loop_run(loop) ...

// teardown, JS task
lvgl_qjs::detach(ctx);              // or detach(ctx, false) before qjs_esp_arena_reset()
qjs_esp_loop_free(loop);
JS_FreeContext(ctx);
JS_FreeRuntime(rt);
```

One context at a time. `detach()` deletes the detached objects. What the
script put in the tree stays there: clean the root to remove it. The firmware
doesn't host a UI script yet; that belongs with `main/ui`.

## SolidJS

`js/solid-lvgl.js` is a Solid universal renderer over this module. JSX
elements are widget types, attributes are properties, text children become
labels (LVGL's way of putting text on a button):

```jsx
import { createSignal, For } from "solid-js";
import { render } from "solid-lvgl";
import * as lv from "lvgl";

function App() {
    const [n, setN] = createSignal(0);
    return (
        <obj width={lv.pct(100)} height={lv.pct(100)} flexFlow={lv.FLEX_FLOW.COLUMN} gap={4}>
            <label text={`Count: ${n()}`} textFont={lv.FONT.MONTSERRAT_28} />
            <button onClick={() => setN(n() + 1)} style={{ bgColor: 0x2d5a87 }}>Add</button>
        </obj>
    );
}
const dispose = render(() => <App />, lv.root());
```

JSX is compiled on a computer, never on the device:

```sh
cd components/lvgl_qjs/js && npm install
node build.mjs path/to/app.jsx app.js   # babel-preset-solid (universal) + esbuild, one ES module
```

The bundle (`lvgl` left as an import) is a plain module the device can run,
or compile to bytecode first with esp-quickjs-ng's `tools/qjs_esp_bc.py`. The
host test's app, Solid included, is 16 KB minified.

Notes:
* A `<label>` shows its `text` prop. Text children are for containers.
* Solid's placeholder for "nothing to show" is an empty text node; it is
  hidden, so it takes no room in a layout.
* `render()`'s dispose also deletes what was rendered.

## Testing on a computer

`host_test/` runs the binding against LVGL with a dummy display, QuickJS-NG
and a fake event loop, under AddressSanitizer and UBSan. QuickJS's teardown
asserts that no JS object leaked.

```sh
B=/tmp/lvgl_qjs_host
cmake -S components/lvgl_qjs/host_test -B $B -G Ninja && ninja -C $B
$B/lvgl_qjs_host components/lvgl_qjs/host_test/test.js
# Solid: build the app first
node components/lvgl_qjs/js/build.mjs components/lvgl_qjs/host_test/solid_app.jsx $B/solid_app.js
$B/lvgl_qjs_host $B/solid_app.js
```

A test script exports `steps`. The runner calls them in order and lets the
loop run between them. A global `host` plays the LVGL task's part
(`host.send(obj, "click")`, `host.cleanRoot()`, `host.gc()`, ...); see
`host_test/main.cpp`.

## Adding to it

* A widget: one row in `WIDGETS` (`src/props.cpp`), and its text/value
  accessors if it has them.
* A style property: one line in `LVQ_STYLE_LIST`.
* A constant: a row in the matching `*_CONSTS` table.
* An event: `EVENT_CODES` and `EVENT_NAMES` in `src/lvgl_qjs.cpp`.

Not there yet: images, grid layout helpers, focus groups/keypad input,
animations (the UI rules say none), fonts other than the built-in Montserrat
sizes.
