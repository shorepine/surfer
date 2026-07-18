/* modsurfer: hand-written MicroPython binding (DESIGN.md §3 — the API is
 * ~8 node kinds and a few widgets, so a generator would be overkill).
 * Unix port + SDL hal for now; the esp32p4 port swaps the hal init.
 *
 * Python surface:
 *   surfer.init(w=1024, h=600); surfer.tick() -> bool; surfer.keys()
 *   surfer.screen() -> Node; surfer.rgb(r,g,b)
 *   nodes:   group/rect/label/textgrid/scrollview  (Node type)
 *   widgets: slider/knob/checkbox/dropdown         (Widget type)
 *   node.x_pos/.y_pos/.w/.h/.hidden, .add(child), .detach(), .destroy()
 *   widget.value, widget.callback, plus the node properties
 * Capitalized aliases (surfer.Slider is surfer.slider) exist for taste.
 * Callbacks fire from surfer.tick() on the same thread — no marshaling. */
#include <string.h>

#include "py/obj.h"
#include "py/objstr.h"
#include "py/runtime.h"

#include "surfer.h"
#include "surfer_port.h"
#include "widget_assets.h"
#include "font_ui16.h"
#include "font_ui28.h"
#include "font_mono16.h"

/* ---- baked default style; pixels re-homed by surfer_port_prepare_image
 * at init (flash .rodata → PSRAM on device, no-op on desktop) ---- */

static surf_image knob_img = {
    .pixels = (void *)widget_knob_px, .w = WKNOB_STRIP_W, .h = WKNOB_SIZE,
    .stride = WKNOB_STRIP_W * 4, .format = SURF_FMT_ARGB8888,
};
static surf_image track_img = {
    .pixels = (void *)widget_trackfull_px, .w = WTRACKFULL_W, .h = WTRACKFULL_H,
    .stride = WTRACKFULL_W * 4, .format = SURF_FMT_ARGB8888,
};
static surf_image cap_img = {
    .pixels = (void *)widget_cap_px, .w = WCAP_W, .h = WCAP_H,
    .stride = WCAP_W * 4, .format = SURF_FMT_ARGB8888,
};
static surf_image check_img = {
    .pixels = (void *)widget_check_px, .w = WCHECK_SIZE * 2, .h = WCHECK_SIZE,
    .stride = WCHECK_SIZE * 2 * 4, .format = SURF_FMT_ARGB8888,
};
static surf_image panel_img = {
    .pixels = (void *)widget_panel_px, .w = WPANEL_SIZE, .h = WPANEL_SIZE,
    .stride = WPANEL_SIZE * 4, .format = SURF_FMT_ARGB8888,
};
static surf_image btn_img = {
    .pixels = (void *)widget_btn_px, .w = WBTN_SIZE, .h = WBTN_SIZE,
    .stride = WBTN_SIZE * 4, .format = SURF_FMT_ARGB8888,
};
static surf_image btnpr_img = {
    .pixels = (void *)widget_btnpr_px, .w = WBTN_SIZE, .h = WBTN_SIZE,
    .stride = WBTN_SIZE * 4, .format = SURF_FMT_ARGB8888,
};
static surf_image knobsm_img = {
    .pixels = (void *)widget_knobsm_px, .w = WKNOBSM_STRIP_W, .h = WKNOBSM_SIZE,
    .stride = WKNOBSM_STRIP_W * 4, .format = SURF_FMT_ARGB8888,
};
static surf_image arrow_img = {
    .pixels = (void *)widget_arrow_px, .w = WARROW_W * 2, .h = WARROW_H,
    .stride = WARROW_W * 2 * 4, .format = SURF_FMT_ARGB8888,
};

/* runtime font copies so the atlases can be re-homed too */
static surf_font fonts_rt[3];
static const surf_font *const fonts_baked[] = {&surf_font_ui16, &surf_font_ui28,
                                               &surf_font_mono16};
#define NFONTS 3

static void prepare_assets(void)
{
    surfer_port_prepare_image(&knob_img);
    surfer_port_prepare_image(&track_img);
    surfer_port_prepare_image(&cap_img);
    surfer_port_prepare_image(&check_img);
    surfer_port_prepare_image(&panel_img);
    surfer_port_prepare_image(&arrow_img);
    surfer_port_prepare_image(&btn_img);
    surfer_port_prepare_image(&btnpr_img);
    surfer_port_prepare_image(&knobsm_img);
    for (int i = 0; i < NFONTS; i++) {
        fonts_rt[i] = *fonts_baked[i];
        surfer_port_prepare_image(&fonts_rt[i].atlas);
    }
}

/* ---- object types ---- */

typedef struct {
    mp_obj_base_t base;
    surf_node *node;
    mp_obj_t touch_cb;  /* node.on_touch: fn(phase, x, y) or None */
} surfer_node_obj_t;

enum { W_SLIDER, W_KNOB, W_CHECKBOX, W_DROPDOWN, W_BUTTON };

typedef struct {
    mp_obj_base_t base;
    uint8_t kind;
    void *w;          /* surf_slider* / surf_knob* / ... */
    surf_node *node;  /* widget root, for tree/pos ops */
    mp_obj_t callback;
} surfer_widget_obj_t;

extern const mp_obj_type_t surfer_node_type;
extern const mp_obj_type_t surfer_widget_type;

/* everything Python creates stays reachable from here so the GC can't
 * collect an object the C side still points at (callbacks, tree links) */
MP_REGISTER_ROOT_POINTER(mp_obj_t surfer_registry);

static void registry_add(mp_obj_t o)
{
    if (MP_STATE_VM(surfer_registry) == MP_OBJ_NULL)
        MP_STATE_VM(surfer_registry) = mp_obj_new_list(0, NULL);
    mp_obj_list_append(MP_STATE_VM(surfer_registry), o);
}

static surfer_node_obj_t *new_node_obj(surf_node *n)
{
    if (!n)
        mp_raise_msg(&mp_type_RuntimeError, MP_ERROR_TEXT("node pool exhausted"));
    surfer_node_obj_t *o = mp_obj_malloc(surfer_node_obj_t, &surfer_node_type);
    o->node = n;
    o->touch_cb = mp_const_none;
    registry_add(MP_OBJ_FROM_PTR(o));
    return o;
}

static surf_node *node_of(mp_obj_t o)
{
    const mp_obj_type_t *t = mp_obj_get_type(o);
    if (t == &surfer_node_type)
        return ((surfer_node_obj_t *)MP_OBJ_TO_PTR(o))->node;
    if (t == &surfer_widget_type)
        return ((surfer_widget_obj_t *)MP_OBJ_TO_PTR(o))->node;
    mp_raise_TypeError(MP_ERROR_TEXT("expected a surfer node or widget"));
}

static const surf_font *font_of(mp_int_t i)
{
    if (i < 0 || i >= NFONTS)
        mp_raise_ValueError(MP_ERROR_TEXT("bad font"));
    return &fonts_rt[i];
}

/* ---- Node ---- */

static mp_obj_t node_add(mp_obj_t self_in, mp_obj_t child)
{
    surf_node_add(node_of(self_in), node_of(child));
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_2(node_add_obj, node_add);

static mp_obj_t node_detach(mp_obj_t self_in)
{
    surf_node_detach(node_of(self_in));
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_1(node_detach_obj, node_detach);

static mp_obj_t node_destroy(mp_obj_t self_in)
{
    surfer_node_obj_t *o = MP_OBJ_TO_PTR(self_in);
    surf_node_destroy(o->node);
    o->node = NULL;
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_1(node_destroy_obj, node_destroy);

static mp_obj_t node_set_text(mp_obj_t self_in, mp_obj_t s)
{
    surf_text_set(node_of(self_in), mp_obj_str_get_str(s));
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_2(node_set_text_obj, node_set_text);

static mp_obj_t node_set_row(mp_obj_t self_in, mp_obj_t row, mp_obj_t s)
{
    surf_textgrid_set_row(node_of(self_in), mp_obj_get_int(row),
                          mp_obj_str_get_str(s));
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_3(node_set_row_obj, node_set_row);

static mp_obj_t node_set_cell(size_t n_args, const mp_obj_t *args)
{
    surf_node *g = node_of(args[0]);
    surf_point cs = surf_textgrid_cell_size(g);
    (void)cs;
    uint32_t cp = 0;
    if (mp_obj_is_str(args[3])) {
        const char *s = mp_obj_str_get_str(args[3]);
        int32_t i = 0;
        cp = surf_utf8_first(s); (void)i;
    } else {
        cp = (uint32_t)mp_obj_get_int(args[3]);
    }
    surf_color fg = n_args > 4 ? (surf_color)mp_obj_get_int(args[4]) : 0xffff;
    surf_color bg = n_args > 5 ? (surf_color)mp_obj_get_int(args[5]) : 0x0000;
    surf_textgrid_set_cell(g, mp_obj_get_int(args[1]), mp_obj_get_int(args[2]),
                           cp, fg, bg);
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(node_set_cell_obj, 4, 6, node_set_cell);

static mp_obj_t node_set_wrap(mp_obj_t self_in, mp_obj_t w)
{
    surf_text_set_wrap(node_of(self_in), (int16_t)mp_obj_get_int(w));
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_2(node_set_wrap_obj, node_set_wrap);

static mp_obj_t node_set_align(mp_obj_t self_in, mp_obj_t a)
{
    surf_text_set_align(node_of(self_in), (surf_align)mp_obj_get_int(a));
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_2(node_set_align_obj, node_set_align);

static mp_obj_t node_set_color(mp_obj_t self_in, mp_obj_t c)
{
    surf_rect_set_color(node_of(self_in), (surf_color)mp_obj_get_int(c));
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_2(node_set_color_obj, node_set_color);

static mp_obj_t node_fast_scroll(mp_obj_t self_in, mp_obj_t on)
{
    surf_textgrid_set_fast_scroll(node_of(self_in), mp_obj_is_true(on));
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_2(node_fast_scroll_obj, node_fast_scroll);

static mp_obj_t node_grid_scroll(mp_obj_t self_in, mp_obj_t rows)
{
    surf_textgrid_scroll(node_of(self_in), mp_obj_get_int(rows));
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_2(node_grid_scroll_obj, node_grid_scroll);

static mp_obj_t node_scroll_offset(mp_obj_t self_in)
{
    surf_point p = surf_scrollview_offset(node_of(self_in));
    mp_obj_t t[2] = {MP_OBJ_NEW_SMALL_INT(p.x), MP_OBJ_NEW_SMALL_INT(p.y)};
    return mp_obj_new_tuple(2, t);
}
static MP_DEFINE_CONST_FUN_OBJ_1(node_scroll_offset_obj, node_scroll_offset);

static mp_obj_t node_scroll_to(mp_obj_t self_in, mp_obj_t x, mp_obj_t y)
{
    surf_scrollview_set_offset(node_of(self_in), mp_obj_get_int(x),
                               mp_obj_get_int(y));
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_3(node_scroll_to_obj, node_scroll_to);

static const mp_rom_map_elem_t node_locals_table[] = {
    {MP_ROM_QSTR(MP_QSTR_add), MP_ROM_PTR(&node_add_obj)},
    {MP_ROM_QSTR(MP_QSTR_detach), MP_ROM_PTR(&node_detach_obj)},
    {MP_ROM_QSTR(MP_QSTR_destroy), MP_ROM_PTR(&node_destroy_obj)},
    {MP_ROM_QSTR(MP_QSTR_set_text), MP_ROM_PTR(&node_set_text_obj)},
    {MP_ROM_QSTR(MP_QSTR_set_color), MP_ROM_PTR(&node_set_color_obj)},
    {MP_ROM_QSTR(MP_QSTR_set_wrap), MP_ROM_PTR(&node_set_wrap_obj)},
    {MP_ROM_QSTR(MP_QSTR_set_align), MP_ROM_PTR(&node_set_align_obj)},
    {MP_ROM_QSTR(MP_QSTR_set_row), MP_ROM_PTR(&node_set_row_obj)},
    {MP_ROM_QSTR(MP_QSTR_set_cell), MP_ROM_PTR(&node_set_cell_obj)},
    {MP_ROM_QSTR(MP_QSTR_grid_scroll), MP_ROM_PTR(&node_grid_scroll_obj)},
    {MP_ROM_QSTR(MP_QSTR_fast_scroll), MP_ROM_PTR(&node_fast_scroll_obj)},
    {MP_ROM_QSTR(MP_QSTR_scroll_to), MP_ROM_PTR(&node_scroll_to_obj)},
    {MP_ROM_QSTR(MP_QSTR_scroll_offset), MP_ROM_PTR(&node_scroll_offset_obj)},
};
static MP_DEFINE_CONST_DICT(node_locals_dict, node_locals_table);

static void node_pos_attr(surf_node *n, qstr attr, mp_obj_t *dest,
                          int16_t x, int16_t y, int16_t w, int16_t h)
{
    if (dest[0] == MP_OBJ_NULL) {  /* load */
        switch (attr) {
        case MP_QSTR_x_pos: dest[0] = MP_OBJ_NEW_SMALL_INT(x); return;
        case MP_QSTR_y_pos: dest[0] = MP_OBJ_NEW_SMALL_INT(y); return;
        case MP_QSTR_w: dest[0] = MP_OBJ_NEW_SMALL_INT(w); return;
        case MP_QSTR_h: dest[0] = MP_OBJ_NEW_SMALL_INT(h); return;
        }
        dest[1] = MP_OBJ_SENTINEL;  /* fall through to methods */
        return;
    }
    /* store */
    if (attr == MP_QSTR_x_pos) {
        surf_node_set_pos(n, (int16_t)mp_obj_get_int(dest[1]), y);
        dest[0] = MP_OBJ_NULL;
    } else if (attr == MP_QSTR_y_pos) {
        surf_node_set_pos(n, x, (int16_t)mp_obj_get_int(dest[1]));
        dest[0] = MP_OBJ_NULL;
    } else if (attr == MP_QSTR_hidden) {
        surf_node_set_hidden(n, mp_obj_is_true(dest[1]));
        dest[0] = MP_OBJ_NULL;
    }
}

/* node.on_touch = fn(phase, x, y): the primitive for building custom
 * widgets in Python (step pads, XY controls, ...). Coordinates are
 * screen-absolute; phase is TOUCH_DOWN/MOVE/UP. */
static void node_touch_tramp(surf_node *n, const surf_touch *t, void *user)
{
    (void)n;
    surfer_node_obj_t *o = user;
    if (o->touch_cb == mp_const_none)
        return;
    mp_obj_t args[3] = {
        MP_OBJ_NEW_SMALL_INT(t->phase),
        MP_OBJ_NEW_SMALL_INT(t->x),
        MP_OBJ_NEW_SMALL_INT(t->y),
    };
    mp_call_function_n_kw(o->touch_cb, 3, 0, args);
}

static void node_attr(mp_obj_t self_in, qstr attr, mp_obj_t *dest)
{
    surfer_node_obj_t *o = MP_OBJ_TO_PTR(self_in);
    if (!o->node) {
        if (dest[0] == MP_OBJ_NULL)
            dest[1] = MP_OBJ_SENTINEL;
        return;
    }
    if (dest[0] == MP_OBJ_NULL && attr == MP_QSTR_on_touch) {
        dest[0] = o->touch_cb;
        return;
    }
    if (dest[0] != MP_OBJ_NULL && attr == MP_QSTR_on_touch) {
        o->touch_cb = dest[1];
        surf_node_set_on_touch(o->node, node_touch_tramp, o);
        dest[0] = MP_OBJ_NULL;
        return;
    }
    surf_point p = surf_node_pos(o->node);
    surf_point s = surf_node_size(o->node);
    node_pos_attr(o->node, attr, dest, p.x, p.y, s.x, s.y);
}

MP_DEFINE_CONST_OBJ_TYPE(surfer_node_type, MP_QSTR_Node, MP_TYPE_FLAG_NONE,
                         attr, node_attr, locals_dict, &node_locals_dict);

/* ---- Widget ---- */

static void widget_cb(int32_t value, void *user)
{
    surfer_widget_obj_t *o = user;
    if (o->callback == mp_const_none || o->callback == MP_OBJ_NULL)
        return;
    mp_obj_t arg;
    switch (o->kind) {
    case W_CHECKBOX: arg = mp_obj_new_bool(value != 0); break;
    case W_DROPDOWN: arg = MP_OBJ_NEW_SMALL_INT(value); break;
    case W_BUTTON:   arg = mp_const_true; break;
    default: arg = mp_obj_new_float((mp_float_t)value / SURF_ONE); break;
    }
    mp_call_function_1(o->callback, arg);
}

static void widget_idx_cb(int32_t idx, void *user)
{
    widget_cb(idx, user);
}

static mp_obj_t widget_get_value(surfer_widget_obj_t *o)
{
    switch (o->kind) {
    case W_SLIDER:
        return mp_obj_new_float(
            (mp_float_t)surf_slider_value(o->w) / SURF_ONE);
    case W_KNOB:
        return mp_obj_new_float((mp_float_t)surf_knob_value(o->w) / SURF_ONE);
    case W_CHECKBOX:
        return mp_obj_new_bool(surf_checkbox_checked(o->w));
    case W_BUTTON:
        return mp_const_none;
    default:
        return MP_OBJ_NEW_SMALL_INT(surf_dropdown_selected(o->w));
    }
}

static void widget_set_value(surfer_widget_obj_t *o, mp_obj_t v)
{
    switch (o->kind) {
    case W_SLIDER:
        surf_slider_set_value(o->w, (int32_t)(mp_obj_get_float(v) * SURF_ONE));
        break;
    case W_KNOB:
        surf_knob_set_value(o->w, (int32_t)(mp_obj_get_float(v) * SURF_ONE));
        break;
    case W_CHECKBOX:
        surf_checkbox_set_checked(o->w, mp_obj_is_true(v));
        break;
    case W_BUTTON:
        break;
    default:
        surf_dropdown_set_selected(o->w, mp_obj_get_int(v));
        break;
    }
}

static void widget_attr(mp_obj_t self_in, qstr attr, mp_obj_t *dest)
{
    surfer_widget_obj_t *o = MP_OBJ_TO_PTR(self_in);
    if (dest[0] == MP_OBJ_NULL) {  /* load */
        if (attr == MP_QSTR_value) {
            dest[0] = widget_get_value(o);
            return;
        }
        if (attr == MP_QSTR_callback) {
            dest[0] = o->callback;
            return;
        }
        if (attr == MP_QSTR_node) {
            surfer_node_obj_t *n = new_node_obj(o->node);
            dest[0] = MP_OBJ_FROM_PTR(n);
            return;
        }
    } else {
        if (attr == MP_QSTR_value) {
            widget_set_value(o, dest[1]);
            dest[0] = MP_OBJ_NULL;
            return;
        }
        if (attr == MP_QSTR_callback) {
            o->callback = dest[1];
            dest[0] = MP_OBJ_NULL;
            return;
        }
        if (attr == MP_QSTR_label && o->kind == W_BUTTON) {
            surf_button_set_label(o->w, mp_obj_str_get_str(dest[1]));
            dest[0] = MP_OBJ_NULL;
            return;
        }
    }
    surf_point p = surf_node_pos(o->node);
    surf_point s = surf_node_size(o->node);
    node_pos_attr(o->node, attr, dest, p.x, p.y, s.x, s.y);
}

static mp_obj_t widget_detach(mp_obj_t self_in)
{
    surfer_widget_obj_t *o = MP_OBJ_TO_PTR(self_in);
    surf_node_detach(o->node);
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_1(widget_detach_obj, widget_detach);

static const mp_rom_map_elem_t widget_locals_table[] = {
    {MP_ROM_QSTR(MP_QSTR_detach), MP_ROM_PTR(&widget_detach_obj)},
};
static MP_DEFINE_CONST_DICT(widget_locals_dict, widget_locals_table);

MP_DEFINE_CONST_OBJ_TYPE(surfer_widget_type, MP_QSTR_Widget, MP_TYPE_FLAG_NONE,
                         attr, widget_attr, locals_dict, &widget_locals_dict);

static surfer_widget_obj_t *new_widget_obj(uint8_t kind, void *w, surf_node *node)
{
    if (!w)
        mp_raise_msg(&mp_type_RuntimeError, MP_ERROR_TEXT("widget create failed"));
    surfer_widget_obj_t *o = mp_obj_malloc(surfer_widget_obj_t, &surfer_widget_type);
    o->kind = kind;
    o->w = w;
    o->node = node;
    o->callback = mp_const_none;
    registry_add(MP_OBJ_FROM_PTR(o));
    return o;
}

/* ---- module functions ---- */

static bool inited;

static const surf_hal *g_hal;

static mp_obj_t mod_init(size_t n_args, const mp_obj_t *args)
{
    int16_t w = n_args > 0 ? (int16_t)mp_obj_get_int(args[0]) : 1024;
    int16_t h = n_args > 1 ? (int16_t)mp_obj_get_int(args[1]) : 600;
    surf_config cfg = {.max_nodes = 512, .bg = SURF_RGB(18, 20, 25)};
    if (inited) {
        /* soft reset (or repeat init): the VM dropped every Python object,
         * so rebuild the C scene from scratch on the surviving hal —
         * stale nodes with dangling callbacks must not outlive the VM */
        surf_deinit();
        if (!surf_init(g_hal, w, h, &cfg))
            mp_raise_msg(&mp_type_RuntimeError, MP_ERROR_TEXT("surf re-init failed"));
        return mp_const_none;
    }
    g_hal = surfer_port_init(w, h);
    if (!g_hal)
        mp_raise_msg(&mp_type_RuntimeError, MP_ERROR_TEXT("display init failed"));
    prepare_assets();
    if (!surf_init(g_hal, w, h, &cfg))
        mp_raise_msg(&mp_type_RuntimeError, MP_ERROR_TEXT("surf init failed"));
    inited = true;
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(mod_init_obj, 0, 2, mod_init);

static mp_obj_t mod_tick(void)
{
    if (!surfer_port_pump())
        return mp_const_false;
    surf_tick();
    return mp_const_true;
}
static MP_DEFINE_CONST_FUN_OBJ_0(mod_tick_obj, mod_tick);

static mp_obj_t mod_keys(void)
{
    mp_obj_t list = mp_obj_new_list(0, NULL);
    surfer_key k;
    while (surfer_port_poll_key(&k)) {
        mp_obj_t t[3] = {
            MP_OBJ_NEW_SMALL_INT(k.kind),
            mp_obj_new_str(k.utf8, strlen(k.utf8)),
            mp_obj_new_bool(k.shift),
        };
        mp_obj_list_append(list, mp_obj_new_tuple(3, t));
    }
    return list;
}
static MP_DEFINE_CONST_FUN_OBJ_0(mod_keys_obj, mod_keys);

static mp_obj_t mod_screen(void)
{
    return MP_OBJ_FROM_PTR(new_node_obj(surf_screen()));
}
static MP_DEFINE_CONST_FUN_OBJ_0(mod_screen_obj, mod_screen);

static mp_obj_t mod_rgb(mp_obj_t r, mp_obj_t g, mp_obj_t b)
{
    return MP_OBJ_NEW_SMALL_INT(SURF_RGB(mp_obj_get_int(r), mp_obj_get_int(g),
                                         mp_obj_get_int(b)));
}
static MP_DEFINE_CONST_FUN_OBJ_3(mod_rgb_obj, mod_rgb);

static mp_obj_t mod_group(mp_obj_t x, mp_obj_t y)
{
    return MP_OBJ_FROM_PTR(new_node_obj(
        surf_group_new(mp_obj_get_int(x), mp_obj_get_int(y))));
}
static MP_DEFINE_CONST_FUN_OBJ_2(mod_group_obj, mod_group);

static mp_obj_t mod_rect(size_t n_args, const mp_obj_t *args)
{
    surf_color c = n_args > 4 ? (surf_color)mp_obj_get_int(args[4])
                              : SURF_RGB(96, 103, 120);
    return MP_OBJ_FROM_PTR(new_node_obj(surf_rect_new(
        mp_obj_get_int(args[0]), mp_obj_get_int(args[1]),
        mp_obj_get_int(args[2]), mp_obj_get_int(args[3]), c)));
}
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(mod_rect_obj, 4, 5, mod_rect);

static mp_obj_t mod_label(size_t n_args, const mp_obj_t *args)
{
    surf_color c = n_args > 3 ? (surf_color)mp_obj_get_int(args[3])
                              : SURF_RGB(240, 242, 248);
    const surf_font *f = font_of(n_args > 4 ? mp_obj_get_int(args[4]) : 0);
    return MP_OBJ_FROM_PTR(new_node_obj(surf_text_new(
        f, mp_obj_str_get_str(args[0]), mp_obj_get_int(args[1]),
        mp_obj_get_int(args[2]), c)));
}
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(mod_label_obj, 3, 5, mod_label);

static mp_obj_t mod_textgrid(size_t n_args, const mp_obj_t *args)
{
    surf_color fg = n_args > 2 ? (surf_color)mp_obj_get_int(args[2])
                               : SURF_RGB(200, 205, 215);
    surf_color bg = n_args > 3 ? (surf_color)mp_obj_get_int(args[3])
                               : SURF_RGB(18, 20, 25);
    const surf_font *f = font_of(n_args > 4 ? mp_obj_get_int(args[4]) : 2);
    return MP_OBJ_FROM_PTR(new_node_obj(surf_textgrid_new(
        f, mp_obj_get_int(args[0]), mp_obj_get_int(args[1]), fg, bg)));
}
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(mod_textgrid_obj, 2, 5, mod_textgrid);

static mp_obj_t mod_scrollview(size_t n_args, const mp_obj_t *args)
{
    (void)n_args;
    return MP_OBJ_FROM_PTR(new_node_obj(surf_scrollview_new(
        mp_obj_get_int(args[0]), mp_obj_get_int(args[1]),
        mp_obj_get_int(args[2]), mp_obj_get_int(args[3]))));
}
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(mod_scrollview_obj, 4, 4, mod_scrollview);

static mp_obj_t mod_slider(size_t n_args, const mp_obj_t *args)
{
    static const surf_slider_style st = {.track = &track_img,
                                         .inset = WTRACK_INSET, .cap = &cap_img};
    int16_t w = n_args > 2 ? (int16_t)mp_obj_get_int(args[2]) : WTRACKFULL_W;
    int16_t h = n_args > 3 ? (int16_t)mp_obj_get_int(args[3]) : WTRACKFULL_H;
    surf_slider *s = surf_slider_new(surf_screen(), 0, 0, w, h, &st);
    if (!s)
        mp_raise_msg(&mp_type_RuntimeError, MP_ERROR_TEXT("slider create failed"));
    surf_node *node = surf_slider_node(s);
    surf_node_detach(node);  /* caller decides the parent via .add() */
    surf_node_set_pos(node, (int16_t)mp_obj_get_int(args[0]),
                      (int16_t)mp_obj_get_int(args[1]));
    surfer_widget_obj_t *o = new_widget_obj(W_SLIDER, s, node);
    surf_slider_on_change(s, widget_cb, o);
    return MP_OBJ_FROM_PTR(o);
}
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(mod_slider_obj, 2, 4, mod_slider);

static mp_obj_t mod_knob(size_t n_args, const mp_obj_t *args)
{
    static const surf_knob_style big = {.strip = &knob_img, .frame_w = WKNOB_SIZE,
                                        .frame_h = WKNOB_SIZE,
                                        .frames = WKNOB_FRAMES};
    static const surf_knob_style small = {.strip = &knobsm_img,
                                          .frame_w = WKNOBSM_SIZE,
                                          .frame_h = WKNOBSM_SIZE,
                                          .frames = WKNOB_FRAMES};
    /* third arg: pixel size — anything < 52 gets the small style */
    const surf_knob_style *st =
        (n_args > 2 && mp_obj_get_int(args[2]) < 52) ? &small : &big;
    surf_knob *k = surf_knob_new(surf_screen(), 0, 0, st);
    if (!k)
        mp_raise_msg(&mp_type_RuntimeError, MP_ERROR_TEXT("knob create failed"));
    surf_node *node = surf_knob_node(k);
    surf_node_detach(node);
    surf_node_set_pos(node, (int16_t)mp_obj_get_int(args[0]),
                      (int16_t)mp_obj_get_int(args[1]));
    surfer_widget_obj_t *o = new_widget_obj(W_KNOB, k, node);
    surf_knob_on_change(k, widget_cb, o);
    return MP_OBJ_FROM_PTR(o);
}
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(mod_knob_obj, 2, 3, mod_knob);

static mp_obj_t mod_button(size_t n_args, const mp_obj_t *args)
{
    static surf_button_style st = {
        .normal = &btn_img, .pressed = &btnpr_img, .inset = WBTN_INSET,
        .text_color = SURF_RGB(240, 242, 248),
    };
    st.font = &fonts_rt[0];
    const char *label = n_args > 4 ? mp_obj_str_get_str(args[4]) : "";
    surf_button *b = surf_button_new(surf_screen(), 0, 0,
                                     (int16_t)mp_obj_get_int(args[2]),
                                     (int16_t)mp_obj_get_int(args[3]), &st, label);
    if (!b)
        mp_raise_msg(&mp_type_RuntimeError, MP_ERROR_TEXT("button create failed"));
    surf_node *node = surf_button_node(b);
    surf_node_detach(node);
    surf_node_set_pos(node, (int16_t)mp_obj_get_int(args[0]),
                      (int16_t)mp_obj_get_int(args[1]));
    surfer_widget_obj_t *o = new_widget_obj(W_BUTTON, b, node);
    surf_button_on_press(b, widget_cb, o);
    return MP_OBJ_FROM_PTR(o);
}
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(mod_button_obj, 4, 5, mod_button);

static mp_obj_t mod_checkbox(mp_obj_t x, mp_obj_t y)
{
    static const surf_checkbox_style st = {.strip = &check_img,
                                           .frame_w = WCHECK_SIZE,
                                           .frame_h = WCHECK_SIZE};
    surf_checkbox *c = surf_checkbox_new(surf_screen(), 0, 0, &st);
    if (!c)
        mp_raise_msg(&mp_type_RuntimeError, MP_ERROR_TEXT("checkbox create failed"));
    surf_node *node = surf_checkbox_node(c);
    surf_node_detach(node);
    surf_node_set_pos(node, (int16_t)mp_obj_get_int(x), (int16_t)mp_obj_get_int(y));
    surfer_widget_obj_t *o = new_widget_obj(W_CHECKBOX, c, node);
    surf_checkbox_on_change(c, widget_cb, o);
    return MP_OBJ_FROM_PTR(o);
}
static MP_DEFINE_CONST_FUN_OBJ_2(mod_checkbox_obj, mod_checkbox);

static mp_obj_t mod_dropdown(size_t n_args, const mp_obj_t *args)
{
    static surf_dropdown_style st = {
        .panel = &panel_img, .inset = WPANEL_INSET,
        .text_color = SURF_RGB(240, 242, 248), .hi_color = SURF_RGB(60, 90, 140),
        .arrow = &arrow_img, .arrow_w = WARROW_W, .arrow_h = WARROW_H,
    };
    st.font = &fonts_rt[0];  /* runtime copy with a device-readable atlas */
    size_t len;
    mp_obj_t *items;
    mp_obj_get_array(args[3], &len, &items);
    if (len == 0)
        mp_raise_ValueError(MP_ERROR_TEXT("no items"));
    const char **strs = m_new(const char *, len);
    for (size_t i = 0; i < len; i++)
        strs[i] = mp_obj_str_get_str(items[i]);
    surf_dropdown *d = surf_dropdown_new(surf_screen(), 0, 0,
                                         (int16_t)mp_obj_get_int(args[2]), &st,
                                         strs, (int32_t)len);
    m_del(const char *, strs, len);
    if (!d)
        mp_raise_msg(&mp_type_RuntimeError, MP_ERROR_TEXT("dropdown create failed"));
    surf_node *node = surf_dropdown_node(d);
    surf_node_detach(node);
    surf_node_set_pos(node, (int16_t)mp_obj_get_int(args[0]),
                      (int16_t)mp_obj_get_int(args[1]));
    surfer_widget_obj_t *o = new_widget_obj(W_DROPDOWN, d, node);
    surf_dropdown_on_change(d, widget_idx_cb, o);
    return MP_OBJ_FROM_PTR(o);
}
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(mod_dropdown_obj, 4, 4, mod_dropdown);

/* test/demo hooks */
static mp_obj_t mod_touch(mp_obj_t x, mp_obj_t y, mp_obj_t phase)
{
    surf_touch t = {(int16_t)mp_obj_get_int(x), (int16_t)mp_obj_get_int(y),
                    (uint8_t)mp_obj_get_int(phase)};
    surf_inject_touch(&t);
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_3(mod_touch_obj, mod_touch);

static mp_obj_t mod_screenshot(mp_obj_t path)
{
    return mp_obj_new_bool(surfer_port_screenshot(mp_obj_str_get_str(path)));
}
static MP_DEFINE_CONST_FUN_OBJ_1(mod_screenshot_obj, mod_screenshot);

static const mp_rom_map_elem_t surfer_globals_table[] = {
    {MP_ROM_QSTR(MP_QSTR___name__), MP_ROM_QSTR(MP_QSTR_surfer)},
    {MP_ROM_QSTR(MP_QSTR_init), MP_ROM_PTR(&mod_init_obj)},
    {MP_ROM_QSTR(MP_QSTR_tick), MP_ROM_PTR(&mod_tick_obj)},
    {MP_ROM_QSTR(MP_QSTR_keys), MP_ROM_PTR(&mod_keys_obj)},
    {MP_ROM_QSTR(MP_QSTR_screen), MP_ROM_PTR(&mod_screen_obj)},
    {MP_ROM_QSTR(MP_QSTR_rgb), MP_ROM_PTR(&mod_rgb_obj)},
    {MP_ROM_QSTR(MP_QSTR_group), MP_ROM_PTR(&mod_group_obj)},
    {MP_ROM_QSTR(MP_QSTR_rect), MP_ROM_PTR(&mod_rect_obj)},
    {MP_ROM_QSTR(MP_QSTR_label), MP_ROM_PTR(&mod_label_obj)},
    {MP_ROM_QSTR(MP_QSTR_textgrid), MP_ROM_PTR(&mod_textgrid_obj)},
    {MP_ROM_QSTR(MP_QSTR_scrollview), MP_ROM_PTR(&mod_scrollview_obj)},
    {MP_ROM_QSTR(MP_QSTR_slider), MP_ROM_PTR(&mod_slider_obj)},
    {MP_ROM_QSTR(MP_QSTR_knob), MP_ROM_PTR(&mod_knob_obj)},
    {MP_ROM_QSTR(MP_QSTR_checkbox), MP_ROM_PTR(&mod_checkbox_obj)},
    {MP_ROM_QSTR(MP_QSTR_dropdown), MP_ROM_PTR(&mod_dropdown_obj)},
    {MP_ROM_QSTR(MP_QSTR_button), MP_ROM_PTR(&mod_button_obj)},
    /* capitalized aliases, DESIGN.md §3 taste */
    {MP_ROM_QSTR(MP_QSTR_Group), MP_ROM_PTR(&mod_group_obj)},
    {MP_ROM_QSTR(MP_QSTR_Slider), MP_ROM_PTR(&mod_slider_obj)},
    {MP_ROM_QSTR(MP_QSTR_Knob), MP_ROM_PTR(&mod_knob_obj)},
    {MP_ROM_QSTR(MP_QSTR_Checkbox), MP_ROM_PTR(&mod_checkbox_obj)},
    {MP_ROM_QSTR(MP_QSTR_Dropdown), MP_ROM_PTR(&mod_dropdown_obj)},
    {MP_ROM_QSTR(MP_QSTR_Button), MP_ROM_PTR(&mod_button_obj)},
    {MP_ROM_QSTR(MP_QSTR__touch), MP_ROM_PTR(&mod_touch_obj)},
    {MP_ROM_QSTR(MP_QSTR_screenshot), MP_ROM_PTR(&mod_screenshot_obj)},
    /* key kinds (match surf_sdl_key_kind) */
    {MP_ROM_QSTR(MP_QSTR_KEY_TEXT), MP_ROM_INT(0)},
    {MP_ROM_QSTR(MP_QSTR_KEY_LEFT), MP_ROM_INT(1)},
    {MP_ROM_QSTR(MP_QSTR_KEY_RIGHT), MP_ROM_INT(2)},
    {MP_ROM_QSTR(MP_QSTR_KEY_UP), MP_ROM_INT(3)},
    {MP_ROM_QSTR(MP_QSTR_KEY_DOWN), MP_ROM_INT(4)},
    {MP_ROM_QSTR(MP_QSTR_KEY_PGUP), MP_ROM_INT(5)},
    {MP_ROM_QSTR(MP_QSTR_KEY_PGDN), MP_ROM_INT(6)},
    {MP_ROM_QSTR(MP_QSTR_KEY_HOME), MP_ROM_INT(7)},
    {MP_ROM_QSTR(MP_QSTR_KEY_END), MP_ROM_INT(8)},
    {MP_ROM_QSTR(MP_QSTR_KEY_BACKSPACE), MP_ROM_INT(9)},
    {MP_ROM_QSTR(MP_QSTR_KEY_DELETE), MP_ROM_INT(10)},
    {MP_ROM_QSTR(MP_QSTR_KEY_ENTER), MP_ROM_INT(11)},
    /* fonts */
    {MP_ROM_QSTR(MP_QSTR_FONT_UI16), MP_ROM_INT(0)},
    {MP_ROM_QSTR(MP_QSTR_FONT_UI28), MP_ROM_INT(1)},
    {MP_ROM_QSTR(MP_QSTR_FONT_MONO16), MP_ROM_INT(2)},
    /* touch phases */
    {MP_ROM_QSTR(MP_QSTR_ALIGN_LEFT), MP_ROM_INT(0)},
    {MP_ROM_QSTR(MP_QSTR_ALIGN_CENTER), MP_ROM_INT(1)},
    {MP_ROM_QSTR(MP_QSTR_ALIGN_RIGHT), MP_ROM_INT(2)},
    {MP_ROM_QSTR(MP_QSTR_TOUCH_DOWN), MP_ROM_INT(0)},
    {MP_ROM_QSTR(MP_QSTR_TOUCH_MOVE), MP_ROM_INT(1)},
    {MP_ROM_QSTR(MP_QSTR_TOUCH_UP), MP_ROM_INT(2)},
};
static MP_DEFINE_CONST_DICT(surfer_module_globals, surfer_globals_table);

const mp_obj_module_t surfer_user_cmodule = {
    .base = {&mp_type_module},
    .globals = (mp_obj_dict_t *)&surfer_module_globals,
};

MP_REGISTER_MODULE(MP_QSTR_surfer, surfer_user_cmodule);
