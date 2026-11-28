// The binding's host test. main.cpp runs the steps in order and lets the
// loop run dry between them. Each step throws on the first thing that's wrong.
import * as lv from "lvgl";

function eq(a, b, what) {
    if (a !== b) {
        throw new Error(`${what}: got ${a}, expected ${b}`);
    }
}

function throws(fn, type, what) {
    try {
        fn();
    } catch (e) {
        if (!(e instanceof type)) {
            throw new Error(`${what}: threw ${e}, expected a ${type.name}`);
        }
        return;
    }
    throw new Error(`${what}: didn't throw`);
}

const root = lv.root();
const btn = lv.create("button", root, { width: 120, height: 40, bgColor: 0x2d5a87 });
const label = lv.create("label", btn, { text: "Start", align: lv.ALIGN.CENTER, textFont: lv.FONT.MONTSERRAT_16 });
const slider = lv.create("slider", root, { min: 0, max: 50, value: 10, y: 60, width: lv.pct(80) });

let clicks = 0;
let changes = 0;
let lastValue = -1;
btn.onClick = function (target) {
    clicks++;
    eq(this, btn, "handler this");
    eq(target, btn, "handler argument");
};
slider.onChange = (s) => {
    changes++;
    lastValue = s.value;
};

let box, a, kept, nodesBefore;

function build() {
    eq(String(btn), "[object lv.Obj]", "toStringTag");
    eq(btn.type, "button", "type");
    eq(label.parent, btn, "parent is the same wrapper");
    eq(btn.firstChild, label, "firstChild");
    eq(root.firstChild, btn, "root.firstChild");
    eq(btn.nextSibling, slider, "nextSibling");
    eq(slider.nextSibling, null, "last nextSibling");
    eq(label.text, "Start", "text");
    label.text = "Start"; // same text: no-op
    eq(btn.bgColor, 0x2d5a87, "bgColor");
    eq(btn.width, 120, "width");
    eq(slider.value, 10, "value");
    eq(slider.max, 50, "max");
    eq(label.textFont, lv.FONT.MONTSERRAT_16, "textFont");
    eq(btn.value, undefined, "a button has no value");
    eq(typeof btn.onClick, "function", "handler getter");
    eq(btn.onChange, undefined, "unset handler");
    btn.hidden = true;
    eq(btn.hidden, true, "hidden");
    btn.hidden = false;
    eq(btn.clickable, true, "a button is clickable");

    throws(() => { btn.text = "x"; }, TypeError, "text on a button");
    throws(() => { btn.value = 1; }, TypeError, "value on a button");
    throws(() => lv.create("nope", root), TypeError, "unknown type");
    throws(() => btn.setStyle("nope", 1), RangeError, "unknown style");
    throws(() => { label.textFont = 99; }, RangeError, "bad font");
    throws(() => { btn.onClick = 5; }, TypeError, "handler not a function");
    throws(() => root.remove(), TypeError, "remove the root");
    throws(() => root.delete(), TypeError, "delete the root");
    throws(() => btn.insert(root), RangeError, "an object into its own child");
    throws(() => btn.insert(label, slider), RangeError, "anchor not a child");
    throws(() => { btn.parent = root; }, TypeError, "parent is read-only");

    btn.setStyle("bgColor", 0xff0000, lv.STATE.PRESSED);
    eq(btn.bgColor, 0x2d5a87, "a pressed style leaves the main one");
    slider.setStyle("bgColor", 0x00ff00, lv.PART.INDICATOR);
}

// Two clicks and three slider moves before the script gets to run
function input() {
    host.send(btn, "click");
    host.send(btn, "click");
    for (const v of [20, 30, 42]) {
        slider.value = v;
        host.send(slider, "change");
    }
    host.send(slider, "press"); // no onPress: nothing queued
    eq(host.queued(), 2, "one dispatch per object");
}

function inputHandled() {
    eq(clicks, 1, "two clicks: one call");
    eq(changes, 1, "three changes: one call");
    eq(lastValue, 42, "the last value");
}

function tree() {
    a = lv.create("label", null, { text: "a" });
    const b = lv.create("label");
    const c = lv.create("label");
    eq(a.parent, null, "detached parent");
    eq(a.nextSibling, null, "detached sibling");

    box = lv.create("obj", root, { flexFlow: lv.FLEX_FLOW.COLUMN, gap: 4, pad: 2 });
    box.insert(a);
    box.insert(c);
    box.insert(b, c); // a b c
    eq(box.firstChild, a, "order 1");
    eq(a.nextSibling, b, "order 2");
    eq(b.nextSibling, c, "order 3");
    eq(c.nextSibling, null, "order 4");
    box.insert(c, a); // c a b
    eq(box.firstChild, c, "move first");
    eq(c.nextSibling, a, "move first 2");
    eq(a.nextSibling, b, "move first 3");
    box.insert(c); // a b c
    eq(box.firstChild, a, "move last");
    eq(b.nextSibling, c, "move last 2");
    box.insert(a, c); // b a c: forward, before the anchor
    eq(box.firstChild, b, "move forward");
    eq(b.nextSibling, a, "move forward 2");
    b.remove(); // a c
    eq(b.parent, null, "removed");
    eq(box.childCount, 2, "childCount");
    box.insert(b, c); // a b c
    eq(a.nextSibling, b, "back in");

    eq(box.layout, lv.LAYOUT.FLEX, "flexFlow turns flex on");
    eq(box.padRow, 4, "gap");
    eq(box.padLeft, 2, "pad");

    // A detached subtree with a handler, dropped: the GC deletes it
    let tmp = lv.create("button");
    lv.create("label", tmp, { text: "gone" });
    tmp.onClick = () => {};
    tmp = null;

    kept = lv.create("label", null, { text: "kept" });

    const d = lv.create("label", box);
    d.delete();
    eq(d.deleted, true, "deleted");
    d.delete(); // again: no-op
    throws(() => d.text, ReferenceError, "use after delete");

    // A failed create leaves nothing behind
    const n = box.childCount;
    throws(() => lv.create("label", box, { text: "x", value: 1 }), TypeError, "bad props");
    eq(box.childCount, n, "half-made object deleted");
    host.gc();
}

function afterGc() {
    const [limbo, trash] = host.detached();
    eq(limbo, 1, "only `kept` is detached");
    eq(trash, 0, "trash emptied");
    eq(kept.text, "kept", "kept alive");
    eq(kept.parent, null, "kept detached");
}

function throwing() {
    const bad = lv.create("button", root);
    bad.onClick = () => {
        throw new Error("thrown on purpose by the test");
    };
    host.send(bad, "click"); // reported, and the loop goes on
}

// Detached subtrees whose children's handlers close over them, built the
// way Solid builds (bottom up, detached) and the way it unmounts (remove()):
// the GC collects them, cycles and all.
let cycleBase, keptCard;

function makeCard() {
    const card = lv.create("obj");
    const inner = lv.create("obj", card);
    const ok = lv.create("button", inner);
    ok.onClick = () => { card.hidden = true; };
    return card;
}

function makePanel() {
    const panel = lv.create("obj", root);
    const close = lv.create("button", panel);
    close.onClick = () => panel.delete();
    panel.remove();
}

function cycles() {
    cycleBase = host.nodes();
    makeCard();
    makePanel();
    keptCard = makeCard(); // still referenced: nothing in it may go
    host.gc();
}

function cyclesCollected() {
    const [limbo, trash] = host.detached();
    eq(limbo, 2, "`kept` and `keptCard` stay detached");
    eq(trash, 0, "trash emptied");
    eq(host.nodes(), cycleBase + 3, "only keptCard's three wrappers left");
    const ok = keptCard.firstChild.firstChild;
    eq(typeof ok.onClick, "function", "a kept subtree keeps its handlers");
    root.insert(keptCard);
    host.send(ok, "click");
}

function keptCardClicked() {
    eq(keptCard.hidden, true, "the handler ran");
    keptCard.delete();
    keptCard = null;
}

// The loop's queue is full when the click comes: the retry timer posts it
let retried = 0;
let rbtn;

function queueFull() {
    rbtn = lv.create("button", root);
    rbtn.onClick = () => retried++;
    host.fill();
    host.send(rbtn, "click");
}

function queueFullRetried() {
    eq(retried, 0, "nothing ran while the queue was full");
    host.tick(50);
}

function queueFullHandled() {
    eq(retried, 1, "the retry posted the click");
    rbtn.delete();
}

function handlersOnDeleted() {
    const x = lv.create("button", root);
    x.delete();
    throws(() => { x.onClick = () => {}; }, ReferenceError, "a handler on a deleted object");
    x.onClick = null; // dropping one is fine
    eq(x.onClick, undefined, "nothing stored");
}

function nullishStyles() {
    const o = lv.create("obj", root);
    const bg = o.bgColor;
    const padLeft = o.padLeft;
    const layout = o.layout;
    o.bgColor = 0x123456;
    eq(o.bgColor, 0x123456, "set");
    o.bgColor = undefined;
    eq(o.bgColor, bg, "undefined: back to the theme's");
    o.pad = 3;
    o.pad = null;
    eq(o.padLeft, padLeft, "null pad: all four sides back");
    o.flexFlow = lv.FLEX_FLOW.ROW;
    o.flexFlow = undefined;
    eq(o.layout, layout, "no flex flow: no flex layout");
    o.setStyle("bgColor", 0x00ff00, lv.STATE.PRESSED);
    o.setStyle("bgColor", null, lv.STATE.PRESSED);
    o.delete();
}

// An earlier handler deletes the object: the later ones of the same batch don't run
let pressSeen = 0;
let clickSeen = 0;

function deleteInHandler() {
    const x = lv.create("button", root);
    x.onPress = () => { pressSeen++; x.delete(); };
    x.onClick = () => { clickSeen++; };
    host.send(x, "press");
    host.send(x, "click");
}

function deleteInHandlerDone() {
    eq(pressSeen, 1, "onPress ran");
    eq(clickSeen, 0, "onClick of a deleted object");
}

// The host empties its root from C: the wrappers find out
function wipe() {
    nodesBefore = host.nodes();
    host.cleanRoot();
}

function wiped() {
    eq(box.deleted, true, "box");
    eq(a.deleted, true, "a");
    eq(btn.deleted, true, "btn");
    eq(kept.deleted, false, "kept was detached, not in the root");
    eq(root.childCount, 0, "root empty");
    host.gc();
}

function nodesFreed() {
    const now = host.nodes();
    if (now >= nodesBefore) {
        throw new Error(`nodes: ${now} after the clean, ${nodesBefore} before`);
    }
}

// Last: a dispatch still queued when the host detaches. The teardown's
// loop run must free its node without calling the script.
function queuedAtDetach() {
    const x = lv.create("button", root);
    x.onClick = () => { throw new Error("a handler ran after detach()"); };
    host.send(x, "click");
    host.holdLoop();
}

export const steps = [
    build, input, inputHandled, tree, afterGc, throwing,
    cycles, cyclesCollected, keptCardClicked,
    queueFull, queueFullRetried, queueFullHandled,
    handlersOnDeleted, nullishStyles, deleteInHandler, deleteInHandlerDone,
    wipe, wiped, nodesFreed, queuedAtDetach,
];
