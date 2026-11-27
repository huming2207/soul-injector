// SolidJS through solid-lvgl.js on the binding: build with js/build.mjs, then
// run the bundle in the host test like test.js.
import { createSignal, For, Show } from "solid-js";
import { render } from "solid-lvgl";
import * as lv from "lvgl";

function eq(a, b, what) {
    if (a !== b) {
        throw new Error(`${what}: got ${a}, expected ${b}`);
    }
}

const [count, setCount] = createSignal(0);
const [items, setItems] = createSignal(["a", "b", "c"]);
const [shown, setShown] = createSignal(true);
let column;
let counter;
let add;

function App() {
    return (
        <obj ref={column} width={lv.pct(100)} height={lv.pct(100)} flexFlow={lv.FLEX_FLOW.COLUMN} gap={4}>
            <label ref={counter} text={`Count: ${count()}`} textFont={lv.FONT.MONTSERRAT_16} />
            <button ref={add} onClick={() => setCount(count() + 1)} style={{ bgColor: 0x2d5a87, height: 32 }}>
                Add
            </button>
            <Show when={shown()}>
                <label text="Ready" />
            </Show>
            <For each={items()}>{(item) => <label text={item} />}</For>
        </obj>
    );
}

const root = lv.root();
const dispose = render(() => <App />, root);

// The texts of the visible children after the button, in order
function texts() {
    const out = [];
    for (let n = add.nextSibling; n; n = n.nextSibling) {
        if (!n.hidden) {
            out.push(n.text);
        }
    }
    return out.join(",");
}

function rendered() {
    eq(root.firstChild, column, "column in the root");
    eq(column.firstChild, counter, "counter first");
    eq(counter.text, "Count: 0", "counter text");
    eq(add.type, "button", "button");
    eq(add.bgColor, 0x2d5a87, "style prop");
    eq(add.firstChild.text, "Add", "button text as a label");
    eq(texts(), "Ready,a,b,c", "children");
}

function click() {
    host.send(add, "click");
    host.send(add, "click");
}

function clicked() {
    eq(counter.text, "Count: 1", "two clicks before the script ran: one call");
    host.send(add, "click");
}

function clickedAgain() {
    eq(counter.text, "Count: 2", "next click");
    setItems(["c", "a", "d"]);
    eq(texts(), "Ready,c,a,d", "For reorders and replaces");
    setShown(false);
    eq(texts(), "c,a,d", "Show hides");
    setShown(true);
    eq(texts(), "Ready,c,a,d", "Show shows");
    setItems([]);
    eq(texts(), "Ready", "For empties");
    host.gc();
}

function collected() {
    const [limbo, trash] = host.detached();
    eq(trash, 0, "trash emptied");
    eq(limbo <= 1, true, `removed nodes collected (limbo ${limbo})`); // Show may keep its last child
}

function disposed() {
    dispose();
    eq(root.childCount, 0, "dispose empties the root");
    host.gc();
}

export const steps = [rendered, click, clicked, clickedAgain, collected, disposed];
