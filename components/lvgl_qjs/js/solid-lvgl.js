// SolidJS on LVGL: Solid's universal renderer over the "lvgl" module.
//
// JSX elements are LVGL widgets by their lv.create() type (<obj>, <label>,
// <button>, <bar>...); attributes are lv.Obj properties (text, value,
// width, bgColor, flexFlow, onClick...). Text children become labels, the
// way LVGL puts text on a button:
//
//   <button onClick={() => setN(n() + 1)} width={120}>Add</button>
//
// A label shows its own `text`: <label text={`Count: ${n()}`} />.
// style={{ bgColor: 0x223344 }} sets several style properties at once.
// An empty text node (Solid's placeholder where <Show> has nothing to show)
// is hidden, so it takes no room in a flex layout.
//
// Compile on a computer: babel-preset-solid with
// { generate: "universal", moduleName: "solid-lvgl" }, then bundle with
// "lvgl" left external. See README.md.
import { createRenderer } from "solid-js/universal";
import * as lv from "lvgl";

const TEXT = Symbol("text node");

function setProperty(node, name, value) {
    if (name === "style") {
        for (const key in value) {
            node[key] = value[key];
        }
    } else if (name === "text") {
        node.text = value == null ? "" : String(value);
    } else {
        node[name] = value;
    }
}

const renderer = createRenderer({
    createElement(type) {
        return lv.create(type);
    },
    createTextNode(value) {
        const text = String(value);
        const node = lv.create("label", null, { text, hidden: text === "" });
        node[TEXT] = true;
        return node;
    },
    replaceText(node, value) {
        const text = String(value);
        node.text = text;
        node.hidden = text === "";
    },
    isTextNode(node) {
        return node[TEXT] === true;
    },
    setProperty,
    insertNode(parent, node, anchor) {
        parent.insert(node, anchor);
    },
    removeNode(parent, node) {
        node.remove(); // detached; LVGL deletes it when nothing references it
    },
    getParentNode(node) {
        return node.parent;
    },
    getFirstChild(node) {
        return node.firstChild;
    },
    getNextSibling(node) {
        return node.nextSibling;
    },
});

// What babel-preset-solid's output imports from "solid-lvgl"
export const {
    effect,
    memo,
    createComponent,
    createElement,
    createTextNode,
    insertNode,
    insert,
    spread,
    setProp,
    mergeProps,
    use,
} = renderer;

// render(() => <App />, lv.root()): returns dispose(), which also deletes
// what was rendered (Solid's universal render leaves it in place).
export function render(code, root) {
    const dispose = renderer.render(code, root);
    return () => {
        dispose();
        for (let n = root.firstChild; n; n = root.firstChild) {
            n.delete();
        }
    };
}
