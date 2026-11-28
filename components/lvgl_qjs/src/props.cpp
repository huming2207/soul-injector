#include <cstring>

#include "lvgl_qjs_priv.hpp"

// What lv.Obj shows: widget properties (text, value...), local style
// properties (width, bgColor...) and the module's constants. All tables are
// const and live in flash; a property is one getter/setter pair with a magic
// number, found by QuickJS's own property lookup.

namespace lvgl_qjs::priv
{
    namespace
    {
        struct widget {
            const char *name;
            lv_obj_t *(*create)(lv_obj_t *parent);
            const lv_obj_class_t *cls;
        };

        const widget WIDGETS[] = {
            {"obj", lv_obj_create, &lv_obj_class},
#if LV_USE_LABEL
            {"label", lv_label_create, &lv_label_class},
#endif
#if LV_USE_BUTTON
            {"button", lv_button_create, &lv_button_class},
#endif
#if LV_USE_BAR
            {"bar", lv_bar_create, &lv_bar_class},
#endif
#if LV_USE_SLIDER
            {"slider", lv_slider_create, &lv_slider_class},
#endif
#if LV_USE_SWITCH
            {"switch", lv_switch_create, &lv_switch_class},
#endif
#if LV_USE_CHECKBOX
            {"checkbox", lv_checkbox_create, &lv_checkbox_class},
#endif
#if LV_USE_ARC
            {"arc", lv_arc_create, &lv_arc_class},
#endif
#if LV_USE_SPINNER
            {"spinner", lv_spinner_create, &lv_spinner_class},
#endif
#if LV_USE_TEXTAREA
            {"textarea", lv_textarea_create, &lv_textarea_class},
#endif
        };

        const lv_font_t *const FONTS[] = {
#if LV_FONT_MONTSERRAT_12
            &lv_font_montserrat_12,
#endif
#if LV_FONT_MONTSERRAT_14
            &lv_font_montserrat_14,
#endif
#if LV_FONT_MONTSERRAT_16
            &lv_font_montserrat_16,
#endif
#if LV_FONT_MONTSERRAT_28
            &lv_font_montserrat_28,
#endif
#if LV_FONT_MONTSERRAT_32
            &lv_font_montserrat_32,
#endif
#if LV_FONT_MONTSERRAT_48
            &lv_font_montserrat_48,
#endif
        };
        constexpr int32_t FONT_COUNT = sizeof(FONTS) / sizeof(FONTS[0]);

        // lv.FONT.MONTSERRAT_n: an index into FONTS, in the same order
        enum font_index : int32_t {
#if LV_FONT_MONTSERRAT_12
            F_12,
#endif
#if LV_FONT_MONTSERRAT_14
            F_14,
#endif
#if LV_FONT_MONTSERRAT_16
            F_16,
#endif
#if LV_FONT_MONTSERRAT_28
            F_28,
#endif
#if LV_FONT_MONTSERRAT_32
            F_32,
#endif
#if LV_FONT_MONTSERRAT_48
            F_48,
#endif
            F_END,
        };
        static_assert(F_END == FONT_COUNT);

        // ---- Widget properties ----

        enum widget_prop : int16_t { W_TEXT, W_VALUE, W_MIN, W_MAX, W_CHECKED, W_DISABLED, W_HIDDEN, W_CLICKABLE, W_SCROLLABLE, W_CHECKABLE };
        static_assert(LV_PART_MAIN == 0 && LV_STATE_DEFAULT == 0);

        // Text of a label, checkbox or textarea; nullptr for other widgets.
        const char *get_text(const lv_obj_t *o)
        {
#if LV_USE_LABEL
            if (lv_obj_check_type(o, &lv_label_class)) {
                return lv_label_get_text(o);
            }
#endif
#if LV_USE_CHECKBOX
            if (lv_obj_check_type(o, &lv_checkbox_class)) {
                return lv_checkbox_get_text(o);
            }
#endif
#if LV_USE_TEXTAREA
            if (lv_obj_check_type(o, &lv_textarea_class)) {
                return lv_textarea_get_text(o);
            }
#endif
            return nullptr;
        }

        void set_text(lv_obj_t *o, const char *text)
        {
#if LV_USE_LABEL
            if (lv_obj_check_type(o, &lv_label_class)) {
                lv_label_set_text(o, text);
            }
#endif
#if LV_USE_CHECKBOX
            if (lv_obj_check_type(o, &lv_checkbox_class)) {
                lv_checkbox_set_text(o, text);
            }
#endif
#if LV_USE_TEXTAREA
            if (lv_obj_check_type(o, &lv_textarea_class)) {
                lv_textarea_set_text(o, text);
            }
#endif
        }

        // value, min and max of a bar, slider or arc
        struct range {
            int32_t value, min, max;
        };

        bool get_range(const lv_obj_t *o, range *r)
        {
#if LV_USE_BAR
            if (lv_obj_check_type(o, &lv_bar_class)) {
                *r = {lv_bar_get_value(o), lv_bar_get_min_value(o), lv_bar_get_max_value(o)};
                return true;
            }
#endif
#if LV_USE_SLIDER
            if (lv_obj_check_type(o, &lv_slider_class)) {
                *r = {lv_slider_get_value(o), lv_slider_get_min_value(o), lv_slider_get_max_value(o)};
                return true;
            }
#endif
#if LV_USE_ARC
            if (lv_obj_check_type(o, &lv_arc_class)) {
                *r = {lv_arc_get_value(o), lv_arc_get_min_value(o), lv_arc_get_max_value(o)};
                return true;
            }
#endif
            return false;
        }

        void set_range(lv_obj_t *o, const range &r)
        {
#if LV_USE_BAR
            if (lv_obj_check_type(o, &lv_bar_class)) {
                lv_bar_set_range(o, r.min, r.max);
                lv_bar_set_value(o, r.value, LV_ANIM_OFF);
            }
#endif
#if LV_USE_SLIDER
            if (lv_obj_check_type(o, &lv_slider_class)) {
                lv_slider_set_range(o, r.min, r.max);
                lv_slider_set_value(o, r.value, LV_ANIM_OFF);
            }
#endif
#if LV_USE_ARC
            if (lv_obj_check_type(o, &lv_arc_class)) {
                lv_arc_set_range(o, r.min, r.max);
                lv_arc_set_value(o, r.value);
            }
#endif
        }

        // hidden, clickable, scrollable, checkable: the last four widget_props, in order
        struct flag_prop {
            bool (*get)(const lv_obj_t *obj);
            void (*set)(lv_obj_t *obj, bool en);
        };
        const flag_prop FLAG_PROPS[] = {
            {lv_obj_is_hidden, lv_obj_set_hidden},
            {lv_obj_is_clickable, lv_obj_set_clickable},
            {lv_obj_is_scrollable, lv_obj_set_scrollable},
            {lv_obj_is_checkable, lv_obj_set_checkable},
        };

        JSValue get_widget_prop(JSContext *ctx, JSValueConst this_val, int magic)
        {
            lv_lock l;
            if (!l.ok) {
                return lock_error(ctx);
            }
            lv_obj_t *o = obj_of(ctx, this_val);
            if (!o) {
                return JS_EXCEPTION;
            }
            range r;
            switch (magic) {
            case W_TEXT: {
                const char *t = get_text(o);
                return t ? JS_NewString(ctx, t) : JS_UNDEFINED;
            }
            case W_VALUE:
                return get_range(o, &r) ? JS_NewInt32(ctx, r.value) : JS_UNDEFINED;
            case W_MIN:
                return get_range(o, &r) ? JS_NewInt32(ctx, r.min) : JS_UNDEFINED;
            case W_MAX:
                return get_range(o, &r) ? JS_NewInt32(ctx, r.max) : JS_UNDEFINED;
            case W_CHECKED:
                return JS_NewBool(ctx, lv_obj_has_state(o, LV_STATE_CHECKED));
            case W_DISABLED:
                return JS_NewBool(ctx, lv_obj_has_state(o, LV_STATE_DISABLED));
            default:
                return JS_NewBool(ctx, FLAG_PROPS[magic - W_HIDDEN].get(o));
            }
        }

        JSValue set_widget_text(JSContext *ctx, JSValueConst this_val, JSValueConst val)
        {
            const char *text = JS_ToCString(ctx, val);
            if (!text) {
                return JS_EXCEPTION;
            }
            JSValue ret = JS_UNDEFINED;
            {
                lv_lock l;
                lv_obj_t *o = l.ok ? obj_of(ctx, this_val) : nullptr;
                const char *cur = o ? get_text(o) : nullptr;
                if (!l.ok) {
                    ret = lock_error(ctx);
                } else if (!o) {
                    ret = JS_EXCEPTION;
                } else if (!cur) {
                    ret = JS_ThrowTypeError(ctx, "a %s has no text", type_name(o));
                } else if (strcmp(cur, text) != 0) { // same text: no redraw
                    set_text(o, text);
                }
            }
            JS_FreeCString(ctx, text);
            return ret;
        }

        JSValue set_widget_prop(JSContext *ctx, JSValueConst this_val, JSValueConst val, int magic)
        {
            if (magic == W_TEXT) {
                return set_widget_text(ctx, this_val, val);
            }
            int32_t num = 0;
            bool on = false;
            if (magic == W_VALUE || magic == W_MIN || magic == W_MAX) {
                if (JS_ToInt32(ctx, &num, val)) {
                    return JS_EXCEPTION;
                }
            } else {
                on = JS_ToBool(ctx, val);
            }

            lv_lock l;
            if (!l.ok) {
                return lock_error(ctx);
            }
            lv_obj_t *o = obj_of(ctx, this_val);
            if (!o) {
                return JS_EXCEPTION;
            }
            range r;
            switch (magic) {
            case W_VALUE:
            case W_MIN:
            case W_MAX:
                if (!get_range(o, &r)) {
                    return JS_ThrowTypeError(ctx, "a %s has no value", type_name(o));
                }
                if (magic == W_VALUE) {
                    r.value = num;
                } else if (magic == W_MIN) {
                    r.min = num;
                } else {
                    r.max = num;
                }
                set_range(o, r);
                break;
            case W_CHECKED:
                lv_obj_set_state(o, LV_STATE_CHECKED, on);
                break;
            case W_DISABLED:
                lv_obj_set_state(o, LV_STATE_DISABLED, on);
                break;
            default:
                FLAG_PROPS[magic - W_HIDDEN].set(o, on);
                break;
            }
            return JS_UNDEFINED;
        }

        // ---- Style properties ----
        //
        // Local styles of the object, as lv_obj_set_style_*() sets them. The
        // accessors work on the main part in the default state; obj.setStyle()
        // takes any selector. magic = kind << 8 | LV_STYLE_* id.

        constexpr lv_style_selector_t SEL_MAIN = 0; // LV_PART_MAIN | LV_STATE_DEFAULT

        enum style_kind : int16_t {
            K_NUM,   // int32: coordinates (lv.pct(), lv.SIZE_CONTENT), opacity, enums
            K_COLOR, // 0xRRGGBB
            K_FONT,  // lv.FONT.*
            K_FLOW,  // flex flow; also turns the flex layout on
            K_PAD,   // pad: all four sides
            K_GAP,   // gap: padRow and padColumn
        };

#define LVQ_STYLE(name, prop, kind) JS_CGETSET_MAGIC_DEF(name, get_style_prop, set_style_prop, (kind) << 8 | (prop))
#define LVQ_STYLE_LIST(X)                                          \
    X("x", LV_STYLE_X, K_NUM)                                      \
    X("y", LV_STYLE_Y, K_NUM)                                      \
    X("width", LV_STYLE_WIDTH, K_NUM)                              \
    X("height", LV_STYLE_HEIGHT, K_NUM)                            \
    X("minWidth", LV_STYLE_MIN_WIDTH, K_NUM)                       \
    X("maxWidth", LV_STYLE_MAX_WIDTH, K_NUM)                       \
    X("minHeight", LV_STYLE_MIN_HEIGHT, K_NUM)                     \
    X("maxHeight", LV_STYLE_MAX_HEIGHT, K_NUM)                     \
    X("align", LV_STYLE_ALIGN, K_NUM)                              \
    X("pad", LV_STYLE_PAD_TOP, K_PAD)                              \
    X("padTop", LV_STYLE_PAD_TOP, K_NUM)                           \
    X("padBottom", LV_STYLE_PAD_BOTTOM, K_NUM)                     \
    X("padLeft", LV_STYLE_PAD_LEFT, K_NUM)                         \
    X("padRight", LV_STYLE_PAD_RIGHT, K_NUM)                       \
    X("gap", LV_STYLE_PAD_ROW, K_GAP)                              \
    X("padRow", LV_STYLE_PAD_ROW, K_NUM)                           \
    X("padColumn", LV_STYLE_PAD_COLUMN, K_NUM)                     \
    X("bgColor", LV_STYLE_BG_COLOR, K_COLOR)                       \
    X("bgOpa", LV_STYLE_BG_OPA, K_NUM)                             \
    X("bgGradColor", LV_STYLE_BG_GRAD_COLOR, K_COLOR)              \
    X("bgGradDir", LV_STYLE_BG_GRAD_DIR, K_NUM)                    \
    X("borderColor", LV_STYLE_BORDER_COLOR, K_COLOR)               \
    X("borderOpa", LV_STYLE_BORDER_OPA, K_NUM)                     \
    X("borderWidth", LV_STYLE_BORDER_WIDTH, K_NUM)                 \
    X("borderSide", LV_STYLE_BORDER_SIDE, K_NUM)                   \
    X("outlineColor", LV_STYLE_OUTLINE_COLOR, K_COLOR)             \
    X("outlineWidth", LV_STYLE_OUTLINE_WIDTH, K_NUM)               \
    X("outlinePad", LV_STYLE_OUTLINE_PAD, K_NUM)                   \
    X("shadowColor", LV_STYLE_SHADOW_COLOR, K_COLOR)               \
    X("shadowWidth", LV_STYLE_SHADOW_WIDTH, K_NUM)                 \
    X("shadowOpa", LV_STYLE_SHADOW_OPA, K_NUM)                     \
    X("shadowOffsetX", LV_STYLE_SHADOW_OFFSET_X, K_NUM)            \
    X("shadowOffsetY", LV_STYLE_SHADOW_OFFSET_Y, K_NUM)            \
    X("shadowSpread", LV_STYLE_SHADOW_SPREAD, K_NUM)               \
    X("radius", LV_STYLE_RADIUS, K_NUM)                            \
    X("clipCorner", LV_STYLE_CLIP_CORNER, K_NUM)                   \
    X("opa", LV_STYLE_OPA, K_NUM)                                  \
    X("textColor", LV_STYLE_TEXT_COLOR, K_COLOR)                   \
    X("textOpa", LV_STYLE_TEXT_OPA, K_NUM)                         \
    X("textFont", LV_STYLE_TEXT_FONT, K_FONT)                      \
    X("textAlign", LV_STYLE_TEXT_ALIGN, K_NUM)                     \
    X("textLetterSpace", LV_STYLE_TEXT_LETTER_SPACE, K_NUM)        \
    X("textLineSpace", LV_STYLE_TEXT_LINE_SPACE, K_NUM)            \
    X("textDecor", LV_STYLE_TEXT_DECOR, K_NUM)                     \
    X("lineColor", LV_STYLE_LINE_COLOR, K_COLOR)                   \
    X("lineWidth", LV_STYLE_LINE_WIDTH, K_NUM)                     \
    X("lineRounded", LV_STYLE_LINE_ROUNDED, K_NUM)                 \
    X("arcColor", LV_STYLE_ARC_COLOR, K_COLOR)                     \
    X("arcWidth", LV_STYLE_ARC_WIDTH, K_NUM)                       \
    X("arcRounded", LV_STYLE_ARC_ROUNDED, K_NUM)                   \
    X("layout", LV_STYLE_LAYOUT, K_NUM)                            \
    X("flexFlow", LV_STYLE_FLEX_FLOW, K_FLOW)                      \
    X("flexMainPlace", LV_STYLE_FLEX_MAIN_PLACE, K_NUM)            \
    X("flexCrossPlace", LV_STYLE_FLEX_CROSS_PLACE, K_NUM)          \
    X("flexTrackPlace", LV_STYLE_FLEX_TRACK_PLACE, K_NUM)          \
    X("flexGrow", LV_STYLE_FLEX_GROW, K_NUM)

        // JS value to an LVGL style value. false with an exception pending.
        bool to_style_value(JSContext *ctx, style_kind kind, JSValueConst val, lv_style_value_t *out)
        {
            *out = {};
            if (kind == K_COLOR) {
                uint32_t rgb;
                if (JS_ToUint32(ctx, &rgb, val)) {
                    return false;
                }
                out->color = lv_color_hex(rgb);
                return true;
            }
            int32_t num;
            if (JS_ToInt32(ctx, &num, val)) {
                return false;
            }
            if (kind != K_FONT) {
                out->num = num;
                return true;
            }
            if (num < 0 || num >= FONT_COUNT) {
                JS_ThrowRangeError(ctx, "not a font: use lv.FONT");
                return false;
            }
            out->ptr = FONTS[num];
            return true;
        }

        bool same_value(style_kind kind, const lv_style_value_t &a, const lv_style_value_t &b)
        {
            switch (kind) {
            case K_COLOR:
                return lv_color_eq(a.color, b.color);
            case K_FONT:
                return a.ptr == b.ptr;
            default:
                return a.num == b.num;
            }
        }

        // Skips a value the object already has: setting it again would redraw.
        // v nullptr: drop the local value (undefined/null from JS), so the
        // theme's applies again, as a conditional Solid prop expects.
        void set_local(lv_obj_t *o, lv_style_prop_t prop, style_kind kind, const lv_style_value_t *v, lv_style_selector_t sel)
        {
            lv_style_value_t cur;
            const bool found = lv_obj_get_local_style_prop(o, prop, &cur, sel) == LV_STYLE_RES_FOUND;
            if (!v) {
                if (found) {
                    lv_obj_remove_local_style_prop(o, prop, sel);
                }
                return;
            }
            if (found && same_value(kind, cur, *v)) {
                return;
            }
            lv_obj_set_local_style_prop(o, prop, *v, sel);
        }

        void apply_style(lv_obj_t *o, int magic, const lv_style_value_t *v, lv_style_selector_t sel)
        {
            const auto kind = static_cast<style_kind>(magic >> 8);
            const auto prop = static_cast<lv_style_prop_t>(magic & 0xff);
            switch (kind) {
            case K_PAD:
                set_local(o, LV_STYLE_PAD_TOP, K_NUM, v, sel);
                set_local(o, LV_STYLE_PAD_BOTTOM, K_NUM, v, sel);
                set_local(o, LV_STYLE_PAD_LEFT, K_NUM, v, sel);
                set_local(o, LV_STYLE_PAD_RIGHT, K_NUM, v, sel);
                break;
            case K_GAP:
                set_local(o, LV_STYLE_PAD_ROW, K_NUM, v, sel);
                set_local(o, LV_STYLE_PAD_COLUMN, K_NUM, v, sel);
                break;
            case K_FLOW: {
                lv_style_value_t flex = {};
                flex.num = LV_LAYOUT_FLEX;
                set_local(o, LV_STYLE_LAYOUT, K_NUM, v ? &flex : nullptr, sel);
                set_local(o, prop, K_NUM, v, sel);
                break;
            }
            default:
                set_local(o, prop, kind, v, sel);
                break;
            }
        }

        JSValue get_style_prop(JSContext *ctx, JSValueConst this_val, int magic)
        {
            lv_lock l;
            if (!l.ok) {
                return lock_error(ctx);
            }
            lv_obj_t *o = obj_of(ctx, this_val);
            if (!o) {
                return JS_EXCEPTION;
            }
            const auto kind = static_cast<style_kind>(magic >> 8);
            const lv_style_value_t v = lv_obj_get_style_prop(o, LV_PART_MAIN, static_cast<lv_style_prop_t>(magic & 0xff));
            if (kind == K_COLOR) {
                return JS_NewUint32(ctx, lv_color_to_u32(v.color) & 0xffffff);
            }
            if (kind == K_FONT) {
                for (int32_t i = 0; i < FONT_COUNT; i++) {
                    if (FONTS[i] == v.ptr) {
                        return JS_NewInt32(ctx, i);
                    }
                }
                return JS_UNDEFINED; // a font the script can't name
            }
            return JS_NewInt32(ctx, v.num);
        }

        JSValue set_style_prop(JSContext *ctx, JSValueConst this_val, JSValueConst val, int magic)
        {
            lv_style_value_t v;
            const bool drop = JS_IsUndefined(val) || JS_IsNull(val);
            if (!drop && !to_style_value(ctx, static_cast<style_kind>(magic >> 8), val, &v)) {
                return JS_EXCEPTION;
            }
            lv_lock l;
            if (!l.ok) {
                return lock_error(ctx);
            }
            lv_obj_t *o = obj_of(ctx, this_val);
            if (!o) {
                return JS_EXCEPTION;
            }
            apply_style(o, magic, drop ? nullptr : &v, SEL_MAIN);
            return JS_UNDEFINED;
        }

        const JSCFunctionListEntry PROPS[] = {
            JS_CGETSET_MAGIC_DEF("text", get_widget_prop, set_widget_prop, W_TEXT),
            JS_CGETSET_MAGIC_DEF("value", get_widget_prop, set_widget_prop, W_VALUE),
            JS_CGETSET_MAGIC_DEF("min", get_widget_prop, set_widget_prop, W_MIN),
            JS_CGETSET_MAGIC_DEF("max", get_widget_prop, set_widget_prop, W_MAX),
            JS_CGETSET_MAGIC_DEF("checked", get_widget_prop, set_widget_prop, W_CHECKED),
            JS_CGETSET_MAGIC_DEF("disabled", get_widget_prop, set_widget_prop, W_DISABLED),
            JS_CGETSET_MAGIC_DEF("hidden", get_widget_prop, set_widget_prop, W_HIDDEN),
            JS_CGETSET_MAGIC_DEF("clickable", get_widget_prop, set_widget_prop, W_CLICKABLE),
            JS_CGETSET_MAGIC_DEF("scrollable", get_widget_prop, set_widget_prop, W_SCROLLABLE),
            JS_CGETSET_MAGIC_DEF("checkable", get_widget_prop, set_widget_prop, W_CHECKABLE),
#define X(name, prop, kind) LVQ_STYLE(name, prop, kind),
            LVQ_STYLE_LIST(X)
#undef X
        };

        struct style_name {
            const char *name;
            int16_t magic;
        };

        const style_name STYLE_NAMES[] = {
#define X(name, prop, kind) {name, (kind) << 8 | (prop)},
            LVQ_STYLE_LIST(X)
#undef X
        };

        // ---- Constants ----

#define C(name, value) JS_PROP_INT32_DEF(name, value, JS_PROP_ENUMERABLE)
        const JSCFunctionListEntry ALIGN_CONSTS[] = {
            C("DEFAULT", LV_ALIGN_DEFAULT),
            C("TOP_LEFT", LV_ALIGN_TOP_LEFT),
            C("TOP_MID", LV_ALIGN_TOP_MID),
            C("TOP_RIGHT", LV_ALIGN_TOP_RIGHT),
            C("BOTTOM_LEFT", LV_ALIGN_BOTTOM_LEFT),
            C("BOTTOM_MID", LV_ALIGN_BOTTOM_MID),
            C("BOTTOM_RIGHT", LV_ALIGN_BOTTOM_RIGHT),
            C("LEFT_MID", LV_ALIGN_LEFT_MID),
            C("RIGHT_MID", LV_ALIGN_RIGHT_MID),
            C("CENTER", LV_ALIGN_CENTER),
        };
        const JSCFunctionListEntry FLEX_FLOW_CONSTS[] = {
            C("ROW", LV_FLEX_FLOW_ROW),
            C("COLUMN", LV_FLEX_FLOW_COLUMN),
            C("ROW_WRAP", LV_FLEX_FLOW_ROW_WRAP),
            C("COLUMN_WRAP", LV_FLEX_FLOW_COLUMN_WRAP),
            C("ROW_REVERSE", LV_FLEX_FLOW_ROW_REVERSE),
            C("COLUMN_REVERSE", LV_FLEX_FLOW_COLUMN_REVERSE),
        };
        const JSCFunctionListEntry FLEX_ALIGN_CONSTS[] = {
            C("START", LV_FLEX_ALIGN_START),
            C("END", LV_FLEX_ALIGN_END),
            C("CENTER", LV_FLEX_ALIGN_CENTER),
            C("SPACE_EVENLY", LV_FLEX_ALIGN_SPACE_EVENLY),
            C("SPACE_AROUND", LV_FLEX_ALIGN_SPACE_AROUND),
            C("SPACE_BETWEEN", LV_FLEX_ALIGN_SPACE_BETWEEN),
        };
        const JSCFunctionListEntry LAYOUT_CONSTS[] = {
            C("NONE", LV_LAYOUT_NONE),
            C("FLEX", LV_LAYOUT_FLEX),
            C("GRID", LV_LAYOUT_GRID),
        };
        const JSCFunctionListEntry TEXT_ALIGN_CONSTS[] = {
            C("AUTO", LV_TEXT_ALIGN_AUTO),
            C("LEFT", LV_TEXT_ALIGN_LEFT),
            C("CENTER", LV_TEXT_ALIGN_CENTER),
            C("RIGHT", LV_TEXT_ALIGN_RIGHT),
        };
        const JSCFunctionListEntry TEXT_DECOR_CONSTS[] = {
            C("NONE", LV_TEXT_DECOR_NONE),
            C("UNDERLINE", LV_TEXT_DECOR_UNDERLINE),
            C("STRIKETHROUGH", LV_TEXT_DECOR_STRIKETHROUGH),
        };
        const JSCFunctionListEntry GRAD_DIR_CONSTS[] = {
            C("NONE", LV_GRAD_DIR_NONE),
            C("VER", LV_GRAD_DIR_VER),
            C("HOR", LV_GRAD_DIR_HOR),
        };
        const JSCFunctionListEntry BORDER_SIDE_CONSTS[] = {
            C("NONE", LV_BORDER_SIDE_NONE),
            C("BOTTOM", LV_BORDER_SIDE_BOTTOM),
            C("TOP", LV_BORDER_SIDE_TOP),
            C("LEFT", LV_BORDER_SIDE_LEFT),
            C("RIGHT", LV_BORDER_SIDE_RIGHT),
            C("FULL", LV_BORDER_SIDE_FULL),
        };
        const JSCFunctionListEntry PART_CONSTS[] = {
            C("MAIN", LV_PART_MAIN),
            C("SCROLLBAR", LV_PART_SCROLLBAR),
            C("INDICATOR", LV_PART_INDICATOR),
            C("KNOB", LV_PART_KNOB),
            C("SELECTED", LV_PART_SELECTED),
            C("ITEMS", LV_PART_ITEMS),
            C("CURSOR", LV_PART_CURSOR),
        };
        const JSCFunctionListEntry STATE_CONSTS[] = {
            C("DEFAULT", LV_STATE_DEFAULT),
            C("CHECKED", LV_STATE_CHECKED),
            C("FOCUSED", LV_STATE_FOCUSED),
            C("FOCUS_KEY", LV_STATE_FOCUS_KEY),
            C("EDITED", LV_STATE_EDITED),
            C("PRESSED", LV_STATE_PRESSED),
            C("DISABLED", LV_STATE_DISABLED),
        };
        const JSCFunctionListEntry FONT_CONSTS[] = {
#if LV_FONT_MONTSERRAT_12
            C("MONTSERRAT_12", F_12),
#endif
#if LV_FONT_MONTSERRAT_14
            C("MONTSERRAT_14", F_14),
#endif
#if LV_FONT_MONTSERRAT_16
            C("MONTSERRAT_16", F_16),
#endif
#if LV_FONT_MONTSERRAT_28
            C("MONTSERRAT_28", F_28),
#endif
#if LV_FONT_MONTSERRAT_32
            C("MONTSERRAT_32", F_32),
#endif
#if LV_FONT_MONTSERRAT_48
            C("MONTSERRAT_48", F_48),
#endif
        };

#define GROUP(name, tab) JS_OBJECT_DEF(name, tab, sizeof(tab) / sizeof(tab[0]), JS_PROP_ENUMERABLE)
        const JSCFunctionListEntry CONSTS[] = {
            C("SIZE_CONTENT", LV_SIZE_CONTENT),
            C("RADIUS_CIRCLE", LV_RADIUS_CIRCLE),
            GROUP("ALIGN", ALIGN_CONSTS),
            GROUP("FLEX_FLOW", FLEX_FLOW_CONSTS),
            GROUP("FLEX_ALIGN", FLEX_ALIGN_CONSTS),
            GROUP("LAYOUT", LAYOUT_CONSTS),
            GROUP("TEXT_ALIGN", TEXT_ALIGN_CONSTS),
            GROUP("TEXT_DECOR", TEXT_DECOR_CONSTS),
            GROUP("GRAD_DIR", GRAD_DIR_CONSTS),
            GROUP("BORDER_SIDE", BORDER_SIDE_CONSTS),
            GROUP("PART", PART_CONSTS),
            GROUP("STATE", STATE_CONSTS),
            GROUP("FONT", FONT_CONSTS),
        };
#undef GROUP
#undef C
    } // namespace

    const JSCFunctionListEntry *const const_list = CONSTS;
    const size_t const_count = sizeof(CONSTS) / sizeof(CONSTS[0]);

    bool init_props(JSContext *ctx, JSValueConst proto)
    {
        return JS_SetPropertyFunctionList(ctx, proto, PROPS, sizeof(PROPS) / sizeof(PROPS[0])) == 0;
    }

    const char *type_name(const lv_obj_t *obj)
    {
        const lv_obj_class_t *cls = lv_obj_get_class(obj);
        for (const widget &w : WIDGETS) {
            if (w.cls == cls) {
                return w.name;
            }
        }
        return "obj"; // a class the binding doesn't know, made by C code
    }

    lv_obj_t *create_widget(const char *type, lv_obj_t *parent)
    {
        for (const widget &w : WIDGETS) {
            if (strcmp(w.name, type) == 0) {
                return w.create(parent);
            }
        }
        return nullptr;
    }

    // obj.setStyle(name, value, selector = lv.PART.MAIN | lv.STATE.DEFAULT)
    JSValue js_set_style(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
    {
        const char *name = JS_ToCString(ctx, argv[0]);
        if (!name) {
            return JS_EXCEPTION;
        }
        int magic = -1;
        for (const style_name &s : STYLE_NAMES) {
            if (strcmp(s.name, name) == 0) {
                magic = s.magic;
                break;
            }
        }
        if (magic < 0) {
            JSValue e = JS_ThrowRangeError(ctx, "setStyle: no style property '%s'", name);
            JS_FreeCString(ctx, name);
            return e;
        }
        JS_FreeCString(ctx, name);

        uint32_t sel = SEL_MAIN;
        lv_style_value_t v;
        const bool drop = JS_IsUndefined(argv[1]) || JS_IsNull(argv[1]);
        if ((!drop && !to_style_value(ctx, static_cast<style_kind>(magic >> 8), argv[1], &v)) ||
            (argc > 2 && !JS_IsUndefined(argv[2]) && JS_ToUint32(ctx, &sel, argv[2]))) {
            return JS_EXCEPTION;
        }
        lv_lock l;
        if (!l.ok) {
            return lock_error(ctx);
        }
        lv_obj_t *o = obj_of(ctx, this_val);
        if (!o) {
            return JS_EXCEPTION;
        }
        apply_style(o, magic, drop ? nullptr : &v, sel);
        return JS_UNDEFINED;
    }
} // namespace lvgl_qjs::priv
