// Types of the "lvgl" module (components/lvgl_qjs).

declare module "lvgl" {
    /** Widget types lv.create() knows; the firmware's LV_USE_* decide which exist. */
    export type WidgetType =
        | "obj" | "label" | "button" | "bar" | "slider" | "switch"
        | "checkbox" | "arc" | "spinner" | "textarea";

    /** A style property's value: a number, a 0xRRGGBB colour, or an lv.FONT index.
     *  Setting undefined or null drops the local value (the theme's applies). */
    export interface StyleProps {
        x: number; y: number; width: number; height: number;
        minWidth: number; maxWidth: number; minHeight: number; maxHeight: number;
        align: number;
        /** All four paddings */
        pad: number;
        padTop: number; padBottom: number; padLeft: number; padRight: number;
        /** padRow and padColumn */
        gap: number;
        padRow: number; padColumn: number;
        bgColor: number; bgOpa: number; bgGradColor: number; bgGradDir: number;
        borderColor: number; borderOpa: number; borderWidth: number; borderSide: number;
        outlineColor: number; outlineWidth: number; outlinePad: number;
        shadowColor: number; shadowWidth: number; shadowOpa: number;
        shadowOffsetX: number; shadowOffsetY: number; shadowSpread: number;
        radius: number; clipCorner: number; opa: number;
        textColor: number; textOpa: number; textFont: number; textAlign: number;
        textLetterSpace: number; textLineSpace: number; textDecor: number;
        lineColor: number; lineWidth: number; lineRounded: number;
        arcColor: number; arcWidth: number; arcRounded: number;
        layout: number;
        /** Also turns the flex layout on */
        flexFlow: number;
        flexMainPlace: number; flexCrossPlace: number; flexTrackPlace: number; flexGrow: number;
    }

    export type Handler = ((this: Obj, target: Obj) => void) | null;

    export interface WidgetProps {
        /** label, checkbox, textarea */
        text: string;
        /** bar, slider, arc */
        value: number; min: number; max: number;
        checked: boolean; disabled: boolean; hidden: boolean;
        clickable: boolean; scrollable: boolean; checkable: boolean;
        onPress: Handler; onLongPress: Handler; onRelease: Handler; onClick: Handler;
        /** LV_EVENT_VALUE_CHANGED */
        onChange: Handler;
        onFocus: Handler; onBlur: Handler;
    }

    export type Props = Partial<{ [K in keyof StyleProps]: StyleProps[K] | null } & WidgetProps>;

    /**
     * One LVGL object. The same object comes back for the same LVGL object
     * (parent, firstChild...). Objects in the screen tree stay alive by
     * themselves; a detached one (no parent, or remove()d) lives while the
     * script references it. Use after delete() throws a ReferenceError.
     */
    export interface Obj extends StyleProps, WidgetProps {
        readonly type: WidgetType;
        readonly parent: Obj | null;
        readonly firstChild: Obj | null;
        readonly nextSibling: Obj | null;
        readonly childCount: number;
        readonly deleted: boolean;
        /** Moves child here, before anchor or at the end (DOM insertBefore). */
        insert(child: Obj, anchor?: Obj | null): void;
        /** Out of the tree; still usable, and insert() puts it back. */
        remove(): void;
        /** Gone, with its children. */
        delete(): void;
        /** A style property for any part and state, e.g. lv.PART.INDICATOR | lv.STATE.PRESSED. */
        setStyle(name: keyof StyleProps, value: number | null | undefined, selector?: number): void;
    }

    export function create(type: WidgetType, parent?: Obj | null, props?: Props): Obj;
    /** The part of the screen the script owns. */
    export function root(): Obj;
    /** n % of the parent, for sizes and positions */
    export function pct(n: number): number;

    export const SIZE_CONTENT: number;
    export const RADIUS_CIRCLE: number;
    export const ALIGN: {
        DEFAULT: number; TOP_LEFT: number; TOP_MID: number; TOP_RIGHT: number;
        BOTTOM_LEFT: number; BOTTOM_MID: number; BOTTOM_RIGHT: number;
        LEFT_MID: number; RIGHT_MID: number; CENTER: number;
    };
    export const FLEX_FLOW: {
        ROW: number; COLUMN: number; ROW_WRAP: number; COLUMN_WRAP: number;
        ROW_REVERSE: number; COLUMN_REVERSE: number;
    };
    export const FLEX_ALIGN: {
        START: number; END: number; CENTER: number;
        SPACE_EVENLY: number; SPACE_AROUND: number; SPACE_BETWEEN: number;
    };
    export const LAYOUT: { NONE: number; FLEX: number; GRID: number };
    export const TEXT_ALIGN: { AUTO: number; LEFT: number; CENTER: number; RIGHT: number };
    export const TEXT_DECOR: { NONE: number; UNDERLINE: number; STRIKETHROUGH: number };
    export const GRAD_DIR: { NONE: number; VER: number; HOR: number };
    export const BORDER_SIDE: {
        NONE: number; BOTTOM: number; TOP: number; LEFT: number; RIGHT: number; FULL: number;
    };
    export const PART: {
        MAIN: number; SCROLLBAR: number; INDICATOR: number; KNOB: number;
        SELECTED: number; ITEMS: number; CURSOR: number;
    };
    export const STATE: {
        DEFAULT: number; CHECKED: number; FOCUSED: number; FOCUS_KEY: number;
        EDITED: number; PRESSED: number; DISABLED: number;
    };
    /** The fonts built into the firmware */
    export const FONT: {
        MONTSERRAT_12: number; MONTSERRAT_14: number; MONTSERRAT_16: number;
        MONTSERRAT_28: number; MONTSERRAT_32: number; MONTSERRAT_48: number;
    };
}

// JSX for solid-lvgl: every widget type is an element taking lv.Obj's props.
declare namespace JSX {
    type Element = import("lvgl").Obj;
    interface ElementChildrenAttribute {
        children: {};
    }
    type LvglProps = Partial<import("lvgl").Props> & {
        ref?: import("lvgl").Obj | ((el: import("lvgl").Obj) => void);
        style?: Partial<import("lvgl").StyleProps>;
        children?: any;
    };
    interface IntrinsicElements {
        obj: LvglProps; label: LvglProps; button: LvglProps; bar: LvglProps;
        slider: LvglProps; switch: LvglProps; checkbox: LvglProps; arc: LvglProps;
        spinner: LvglProps; textarea: LvglProps;
    }
}
