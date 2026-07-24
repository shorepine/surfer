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
    mp_obj_t img_ref;   /* sprites: keeps the Image object alive */
} surfer_node_obj_t;

typedef struct {
    mp_obj_base_t base;
    surf_image *img;    /* NULL after destroy() */
} surfer_image_obj_t;

typedef struct {
    mp_obj_base_t base;
    int idx;            /* pad slot 0..SURF_MAX_PADS-1 */
} surfer_pad_obj_t;
extern const mp_obj_type_t surfer_pad_type;

typedef struct {
    mp_obj_base_t base;
    surf_font *font;    /* NULL after destroy() */
} surfer_font_obj_t;
extern const mp_obj_type_t surfer_font_type;

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
extern const mp_obj_type_t surfer_image_type;

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
    o->img_ref = mp_const_none;
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
    /* each setter no-ops on the wrong node type */
    surf_textgrid_set_fast_scroll(node_of(self_in), mp_obj_is_true(on));
    surf_scrollview_set_fast_scroll(node_of(self_in), mp_obj_is_true(on));
    surf_layer_set_fast_scroll(node_of(self_in), mp_obj_is_true(on));
    surf_sprite_set_fast_pan(node_of(self_in), mp_obj_is_true(on));
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_2(node_fast_scroll_obj, node_fast_scroll);

/* sprite.set_src(x, y, w, h) — the camera-window primitive: with
 * fast_scroll(True) a pan over a big baked image is one band shift */
static mp_obj_t node_set_src(size_t n_args, const mp_obj_t *args)
{
    (void)n_args;
    surf_sprite_set_src(node_of(args[0]), (surf_rect){
        (int16_t)mp_obj_get_int(args[1]), (int16_t)mp_obj_get_int(args[2]),
        (int16_t)mp_obj_get_int(args[3]), (int16_t)mp_obj_get_int(args[4])});
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(node_set_src_obj, 5, 5, node_set_src);

/* layer.set_offset(px) — float pixels; wraps at the strip width */
static mp_obj_t node_set_offset(mp_obj_t self_in, mp_obj_t off)
{
    surf_layer_set_offset(node_of(self_in),
                          (int32_t)(mp_obj_get_float(off) * SURF_ONE));
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_2(node_set_offset_obj, node_set_offset);

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

/* n.damage() — force a repaint of the node's pixels. Needed when the
 * content changed but the node didn't move: e.g. retinting an A8 image
 * for color cycling. */
static mp_obj_t node_damage(mp_obj_t self_in)
{
    surf_node_damage(node_of(self_in));
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_1(node_damage_obj, node_damage);

/* a.hits(b) — do the two nodes' on-screen footprints overlap? */
static mp_obj_t node_hits(mp_obj_t self_in, mp_obj_t other_in)
{
    return mp_obj_new_bool(surf_node_overlaps(node_of(self_in), node_of(other_in)));
}
static MP_DEFINE_CONST_FUN_OBJ_2(node_hits_obj, node_hits);

static const mp_rom_map_elem_t node_locals_table[] = {
    {MP_ROM_QSTR(MP_QSTR_add), MP_ROM_PTR(&node_add_obj)},
    {MP_ROM_QSTR(MP_QSTR_damage), MP_ROM_PTR(&node_damage_obj)},
    {MP_ROM_QSTR(MP_QSTR_hits), MP_ROM_PTR(&node_hits_obj)},
    {MP_ROM_QSTR(MP_QSTR_detach), MP_ROM_PTR(&node_detach_obj)},
    {MP_ROM_QSTR(MP_QSTR_destroy), MP_ROM_PTR(&node_destroy_obj)},
    {MP_ROM_QSTR(MP_QSTR_set_text), MP_ROM_PTR(&node_set_text_obj)},
    {MP_ROM_QSTR(MP_QSTR_set_color), MP_ROM_PTR(&node_set_color_obj)},
    {MP_ROM_QSTR(MP_QSTR_set_wrap), MP_ROM_PTR(&node_set_wrap_obj)},
    {MP_ROM_QSTR(MP_QSTR_set_align), MP_ROM_PTR(&node_set_align_obj)},
    {MP_ROM_QSTR(MP_QSTR_set_row), MP_ROM_PTR(&node_set_row_obj)},
    {MP_ROM_QSTR(MP_QSTR_set_cell), MP_ROM_PTR(&node_set_cell_obj)},
    {MP_ROM_QSTR(MP_QSTR_grid_scroll), MP_ROM_PTR(&node_grid_scroll_obj)},
    {MP_ROM_QSTR(MP_QSTR_set_offset), MP_ROM_PTR(&node_set_offset_obj)},
    {MP_ROM_QSTR(MP_QSTR_set_src), MP_ROM_PTR(&node_set_src_obj)},
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
    /* sprite transform: scale (float, 1.0 = 1:1), rot (degrees CCW,
     * quarter turns only — the P4 PPA's limit), mirror_x / mirror_y
     * (bools; source flip before rotation) */
    if (dest[0] == MP_OBJ_NULL && attr == MP_QSTR_scale) {
        dest[0] = mp_obj_new_float((mp_float_t)surf_sprite_scale(o->node) /
                                   (mp_float_t)SURF_ONE);
        return;
    }
    if (dest[0] == MP_OBJ_NULL && attr == MP_QSTR_rot) {
        dest[0] = MP_OBJ_NEW_SMALL_INT(surf_sprite_rot(o->node) * 90);
        return;
    }
    if (dest[0] == MP_OBJ_NULL && attr == MP_QSTR_mirror_x) {
        dest[0] = mp_obj_new_bool(surf_sprite_mirror(o->node) & 1);
        return;
    }
    if (dest[0] == MP_OBJ_NULL && attr == MP_QSTR_mirror_y) {
        dest[0] = mp_obj_new_bool(surf_sprite_mirror(o->node) & 2);
        return;
    }
    if (dest[0] != MP_OBJ_NULL && attr == MP_QSTR_scale) {
        surf_sprite_set_xform(o->node,
                              (int32_t)(mp_obj_get_float(dest[1]) * SURF_ONE),
                              surf_sprite_rot(o->node),
                              surf_sprite_mirror(o->node));
        dest[0] = MP_OBJ_NULL;
        return;
    }
    if (dest[0] != MP_OBJ_NULL && attr == MP_QSTR_rot) {
        mp_int_t deg = mp_obj_get_int(dest[1]);
        deg = ((deg % 360) + 360) % 360;
        if (deg % 90)
            mp_raise_ValueError(MP_ERROR_TEXT("rot must be a multiple of 90"));
        surf_sprite_set_xform(o->node, surf_sprite_scale(o->node),
                              (uint8_t)(deg / 90),
                              surf_sprite_mirror(o->node));
        dest[0] = MP_OBJ_NULL;
        return;
    }
    if (dest[0] != MP_OBJ_NULL &&
        (attr == MP_QSTR_mirror_x || attr == MP_QSTR_mirror_y)) {
        uint8_t bit = attr == MP_QSTR_mirror_x ? 1 : 2;
        uint8_t m = surf_sprite_mirror(o->node);
        m = mp_obj_is_true(dest[1]) ? (m | bit) : (m & ~bit);
        surf_sprite_set_xform(o->node, surf_sprite_scale(o->node),
                              surf_sprite_rot(o->node), m);
        dest[0] = MP_OBJ_NULL;
        return;
    }
    surf_point p = surf_node_pos(o->node);
    surf_point s = surf_node_size(o->node);
    node_pos_attr(o->node, attr, dest, p.x, p.y, s.x, s.y);
}

MP_DEFINE_CONST_OBJ_TYPE(surfer_node_type, MP_QSTR_Node, MP_TYPE_FLAG_NONE,
                         attr, node_attr, locals_dict, &node_locals_dict);

/* ---- Image (runtime PNG) ---- */

static mp_obj_t image_destroy(mp_obj_t self_in)
{
    surfer_image_obj_t *o = MP_OBJ_TO_PTR(self_in);
    if (o->img) {
        surf_image_destroy(o->img);
        o->img = NULL;
    }
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_1(image_destroy_obj, image_destroy);

static surf_image *image_of(mp_obj_t o)
{
    if (!mp_obj_is_type(o, &surfer_image_type))
        mp_raise_TypeError(MP_ERROR_TEXT("expected surfer Image"));
    surfer_image_obj_t *io = MP_OBJ_TO_PTR(o);
    if (!io->img)
        mp_raise_ValueError(MP_ERROR_TEXT("image destroyed"));
    return io->img;
}

/* img.blit(src, x, y, rot=0) — load-time composition (rot in degrees
 * CCW, quarter turns); never call per frame */
static mp_obj_t image_blit(size_t n_args, const mp_obj_t *args)
{
    surf_image *dst = image_of(args[0]);
    surf_image *src = image_of(args[1]);
    mp_int_t deg = n_args > 4 ? mp_obj_get_int(args[4]) : 0;
    deg = ((deg % 360) + 360) % 360;
    if (deg % 90)
        mp_raise_ValueError(MP_ERROR_TEXT("rot must be a multiple of 90"));
    surf_image_blit_rot(dst, src, (surf_rect){0, 0, src->w, src->h},
                        (int16_t)mp_obj_get_int(args[2]),
                        (int16_t)mp_obj_get_int(args[3]),
                        (uint8_t)(deg / 90));
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(image_blit_obj, 4, 5, image_blit);

/* img.fill(color[, x, y, w, h]) */
static mp_obj_t image_fill(size_t n_args, const mp_obj_t *args)
{
    surf_image *dst = image_of(args[0]);
    surf_rect r = {0, 0, dst->w, dst->h};
    if (n_args >= 6) {
        r = (surf_rect){(int16_t)mp_obj_get_int(args[2]),
                        (int16_t)mp_obj_get_int(args[3]),
                        (int16_t)mp_obj_get_int(args[4]),
                        (int16_t)mp_obj_get_int(args[5])};
    }
    surf_image_fill(dst, r, (surf_color)mp_obj_get_int(args[1]));
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(image_fill_obj, 2, 6, image_fill);

/* ---- shape drawing (load-time; see surf_image_poly & co.) ----
 * paint arg: rgb565 int | (color, alpha) | ((x0,y0,c0[,a0]), (x1,y1,c1[,a1]))
 * for a linear gradient between two stops. */
static surf_paint parse_paint(mp_obj_t o)
{
    surf_paint p = {.kind = SURF_PAINT_SOLID, .a0 = 255, .a1 = 255};
    if (mp_obj_is_int(o)) {
        p.c0 = p.c1 = (surf_color)mp_obj_get_int(o);
        return p;
    }
    size_t n;
    mp_obj_t *it;
    mp_obj_get_array(o, &n, &it);
    if (n == 2 && mp_obj_is_int(it[0])) {          /* (color, alpha) */
        p.c0 = p.c1 = (surf_color)mp_obj_get_int(it[0]);
        p.a0 = p.a1 = (uint8_t)mp_obj_get_int(it[1]);
        return p;
    }
    if (n != 2)
        mp_raise_ValueError(MP_ERROR_TEXT("bad paint"));
    p.kind = SURF_PAINT_LINEAR;
    int32_t *ax = &p.x0;
    for (int i = 0; i < 2; i++) {                  /* two gradient stops */
        size_t sn;
        mp_obj_t *st;
        mp_obj_get_array(it[i], &sn, &st);
        if (sn != 3 && sn != 4)
            mp_raise_ValueError(MP_ERROR_TEXT("gradient stop is (x,y,color[,alpha])"));
        ax[i * 2] = (int32_t)(mp_obj_get_float(st[0]) * 65536);
        ax[i * 2 + 1] = (int32_t)(mp_obj_get_float(st[1]) * 65536);
        surf_color c = (surf_color)mp_obj_get_int(st[2]);
        uint8_t a = sn == 4 ? (uint8_t)mp_obj_get_int(st[3]) : 255;
        if (i == 0) { p.c0 = c; p.a0 = a; } else { p.c1 = c; p.a1 = a; }
    }
    return p;
}

#define Q16F(o) ((int32_t)(mp_obj_get_float(o) * 65536))

/* [(x,y), ...] -> malloc'd Q16 array (caller frees) */
static int32_t *parse_pts(mp_obj_t o, int *count)
{
    size_t n;
    mp_obj_t *it;
    mp_obj_get_array(o, &n, &it);
    if (n < 1 || n > 4096)
        mp_raise_ValueError(MP_ERROR_TEXT("bad point list"));
    int32_t *xy = m_new(int32_t, n * 2);
    for (size_t i = 0; i < n; i++) {
        size_t pn;
        mp_obj_t *pt;
        mp_obj_get_array(it[i], &pn, &pt);
        if (pn != 2)
            mp_raise_ValueError(MP_ERROR_TEXT("points are (x, y)"));
        xy[i * 2] = Q16F(pt[0]);
        xy[i * 2 + 1] = Q16F(pt[1]);
    }
    *count = (int)n;
    return xy;
}

/* img.poly([(x,y),...], paint) — filled, anti-aliased */
static mp_obj_t image_poly(mp_obj_t self_in, mp_obj_t pts_in, mp_obj_t paint_in)
{
    surf_image *dst = image_of(self_in);
    surf_paint p = parse_paint(paint_in);
    int n;
    int32_t *xy = parse_pts(pts_in, &n);
    surf_image_poly(dst, xy, n, &p);
    m_del(int32_t, xy, n * 2);
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_3(image_poly_obj, image_poly);

/* img.lines([(x,y),...], paint, width=1) — round caps and joins */
static mp_obj_t image_lines(size_t n_args, const mp_obj_t *args)
{
    surf_image *dst = image_of(args[0]);
    surf_paint p = parse_paint(args[2]);
    int32_t w = n_args > 3 ? Q16F(args[3]) : 65536;
    int n;
    int32_t *xy = parse_pts(args[1], &n);
    surf_image_polyline(dst, xy, n, w, &p);
    m_del(int32_t, xy, n * 2);
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(image_lines_obj, 3, 4, image_lines);

/* img.line(x0, y0, x1, y1, paint, width=1) */
static mp_obj_t image_line(size_t n_args, const mp_obj_t *args)
{
    surf_image *dst = image_of(args[0]);
    surf_paint p = parse_paint(args[5]);
    int32_t xy[4] = {Q16F(args[1]), Q16F(args[2]), Q16F(args[3]), Q16F(args[4])};
    surf_image_polyline(dst, xy, 2, n_args > 6 ? Q16F(args[6]) : 65536, &p);
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(image_line_obj, 6, 7, image_line);

/* img.ellipse(cx, cy, rx, ry, paint, width=0) — width 0 fills */
static mp_obj_t image_ellipse(size_t n_args, const mp_obj_t *args)
{
    surf_image *dst = image_of(args[0]);
    surf_paint p = parse_paint(args[5]);
    surf_image_ellipse(dst, Q16F(args[1]), Q16F(args[2]),
                       Q16F(args[3]), Q16F(args[4]),
                       n_args > 6 ? Q16F(args[6]) : 0, &p);
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(image_ellipse_obj, 6, 7, image_ellipse);

/* img.circle(cx, cy, r, paint, width=0) */
static mp_obj_t image_circle(size_t n_args, const mp_obj_t *args)
{
    surf_image *dst = image_of(args[0]);
    surf_paint p = parse_paint(args[4]);
    surf_image_ellipse(dst, Q16F(args[1]), Q16F(args[2]),
                       Q16F(args[3]), Q16F(args[3]),
                       n_args > 5 ? Q16F(args[5]) : 0, &p);
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(image_circle_obj, 5, 6, image_circle);

/* img.bezier([(x,y) x3 quadratic | x4 cubic], paint, width=2) */
static mp_obj_t image_bezier(size_t n_args, const mp_obj_t *args)
{
    surf_image *dst = image_of(args[0]);
    surf_paint p = parse_paint(args[2]);
    int n;
    int32_t *xy = parse_pts(args[1], &n);
    if (n != 3 && n != 4) {
        m_del(int32_t, xy, n * 2);
        mp_raise_ValueError(MP_ERROR_TEXT("bezier takes 3 or 4 points"));
    }
    int32_t c[8];
    if (n == 4) {
        memcpy(c, xy, sizeof c);
    } else {  /* elevate quadratic: c1 = p0/3 + 2q/3, c2 = 2q/3 + p1/3 */
        c[0] = xy[0]; c[1] = xy[1];
        c[2] = (int32_t)(xy[0] / 3 + 2LL * xy[2] / 3);
        c[3] = (int32_t)(xy[1] / 3 + 2LL * xy[3] / 3);
        c[4] = (int32_t)(2LL * xy[2] / 3 + xy[4] / 3);
        c[5] = (int32_t)(2LL * xy[3] / 3 + xy[5] / 3);
        c[6] = xy[4]; c[7] = xy[5];
    }
    surf_image_bezier(dst, c, n_args > 3 ? Q16F(args[3]) : 131072, &p);
    m_del(int32_t, xy, n * 2);
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(image_bezier_obj, 3, 4, image_bezier);

static const mp_rom_map_elem_t image_locals_table[] = {
    {MP_ROM_QSTR(MP_QSTR_poly), MP_ROM_PTR(&image_poly_obj)},
    {MP_ROM_QSTR(MP_QSTR_line), MP_ROM_PTR(&image_line_obj)},
    {MP_ROM_QSTR(MP_QSTR_lines), MP_ROM_PTR(&image_lines_obj)},
    {MP_ROM_QSTR(MP_QSTR_circle), MP_ROM_PTR(&image_circle_obj)},
    {MP_ROM_QSTR(MP_QSTR_ellipse), MP_ROM_PTR(&image_ellipse_obj)},
    {MP_ROM_QSTR(MP_QSTR_bezier), MP_ROM_PTR(&image_bezier_obj)},
    {MP_ROM_QSTR(MP_QSTR_destroy), MP_ROM_PTR(&image_destroy_obj)},
    {MP_ROM_QSTR(MP_QSTR_blit), MP_ROM_PTR(&image_blit_obj)},
    {MP_ROM_QSTR(MP_QSTR_fill), MP_ROM_PTR(&image_fill_obj)},
};
static MP_DEFINE_CONST_DICT(image_locals_dict, image_locals_table);

static void image_attr(mp_obj_t self_in, qstr attr, mp_obj_t *dest)
{
    surfer_image_obj_t *o = MP_OBJ_TO_PTR(self_in);
    if (dest[0] == MP_OBJ_NULL && o->img) {
        if (attr == MP_QSTR_w) {
            dest[0] = MP_OBJ_NEW_SMALL_INT(o->img->w);
            return;
        }
        if (attr == MP_QSTR_h) {
            dest[0] = MP_OBJ_NEW_SMALL_INT(o->img->h);
            return;
        }
        if (attr == MP_QSTR_tint) {
            dest[0] = MP_OBJ_NEW_SMALL_INT(o->img->tint);
            return;
        }
    }
    /* store: img.tint = rgb565 (A8 masks: the color the alpha draws in;
     * retint + sprite.damage() per frame = hardware color cycling) */
    if (dest[0] != MP_OBJ_NULL && dest[1] != MP_OBJ_NULL &&
        attr == MP_QSTR_tint && o->img) {
        o->img->tint = (surf_color)mp_obj_get_int(dest[1]);
        dest[0] = MP_OBJ_NULL;
        return;
    }
    if (dest[0] == MP_OBJ_NULL)
        dest[1] = MP_OBJ_SENTINEL;
}

MP_DEFINE_CONST_OBJ_TYPE(surfer_image_type, MP_QSTR_Image, MP_TYPE_FLAG_NONE,
                         attr, image_attr, locals_dict, &image_locals_dict);

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
static int16_t g_scr_w, g_scr_h;

/* surfer.fb_read(x, y, w, h) -> RGB888 bytes — the portable screenshot
 * path: read the framebuffer, write it with Python file IO (the P4's
 * MicroPython VFS is invisible to C fopen). */
static mp_obj_t mod_fb_read(size_t n_args, const mp_obj_t *args)
{
    (void)n_args;
    if (!g_hal || !g_hal->fb_ptr)
        mp_raise_msg(&mp_type_RuntimeError, MP_ERROR_TEXT("no framebuffer"));
    mp_int_t x = mp_obj_get_int(args[0]), y = mp_obj_get_int(args[1]);
    mp_int_t w = mp_obj_get_int(args[2]), h = mp_obj_get_int(args[3]);
    if (x < 0 || y < 0 || w <= 0 || h <= 0 ||
        x + w > g_scr_w || y + h > g_scr_h)
        mp_raise_ValueError(MP_ERROR_TEXT("region out of bounds"));
    surfer_port_fb_sync_for_read();
    int32_t stride;
    const uint8_t *fb = g_hal->fb_ptr(&stride);
    vstr_t out;
    vstr_init_len(&out, (size_t)w * h * 3);
    uint8_t *d = (uint8_t *)out.buf;
    for (mp_int_t j = 0; j < h; j++) {
        const uint16_t *row = (const uint16_t *)(fb + (y + j) * stride) + x;
        for (mp_int_t i = 0; i < w; i++) {
            uint16_t p = row[i];
            *d++ = (uint8_t)(((p >> 8) & 0xf8) | (p >> 13));
            *d++ = (uint8_t)(((p >> 3) & 0xfc) | ((p >> 9) & 0x03));
            *d++ = (uint8_t)(((p << 3) & 0xf8) | ((p >> 2) & 0x07));
        }
    }
    return mp_obj_new_bytes_from_vstr(&out);
}
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(mod_fb_read_obj, 4, 4, mod_fb_read);

/* surfer.frame_rate(fps) — game mode: lock tick to the panel at the
 * nearest divisor of its MEASURED refresh rate, returning the actual
 * locked fps (this panel refreshes at 69.7 Hz, so frame_rate(30) locks
 * 34.8 and frame_rate(60) locks 69.7 — scale per-frame speeds by the
 * return value if it matters). 0 = uncapped (the default), returns the
 * panel rate. Early frames wait on vsync; late frames slip whole
 * periods, so cadence stays quantized instead of wobbling. */
static mp_obj_t mod_frame_rate(mp_obj_t fps_in)
{
    mp_float_t fps = mp_obj_get_float(fps_in);
    float hz = surf_frame_hz();
    if (fps <= 0) {
        surf_set_frame_divisor(0);
        return mp_obj_new_float((mp_float_t)hz);
    }
    int div = (int)(hz / (float)fps + 0.5f);
    if (div < 1)
        div = 1;
    surf_set_frame_divisor(div);
    return mp_obj_new_float((mp_float_t)(hz / (float)div));
}
static MP_DEFINE_CONST_FUN_OBJ_1(mod_frame_rate_obj, mod_frame_rate);

/* surfer.cpu() -> (pct, ...) busy percent per core since the last call
 * (one entry per core on the P4, one process-wide entry on desktop,
 * empty on web). Poll it about once a second alongside an fps meter. */
static mp_obj_t mod_cpu(void)
{
    float pct[4];
    int n = surfer_port_cpu_usage(pct, 4);
    mp_obj_t items[4];
    for (int i = 0; i < n; i++)
        items[i] = mp_obj_new_float((mp_float_t)pct[i]);
    return mp_obj_new_tuple((size_t)n, items);
}
static MP_DEFINE_CONST_FUN_OBJ_0(mod_cpu_obj, mod_cpu);

/* ---- Pad (controller) ----------------------------------------------
 * A handle to one controller slot. GAMES read live state through
 * attributes; SOURCES (drivers, the touch overlay, tests) write it
 * through the set_* methods. The keyboard is wired to a slot for free
 * by surfer.pad_keys(). See surfer.h for the model. */
static mp_float_t pad_axf(int idx, int stick, int axis)
{
    return (mp_float_t)surf_pad_axis(idx, stick, axis) / (mp_float_t)SURF_ONE;
}

static void pad_attr(mp_obj_t self_in, qstr attr, mp_obj_t *dest)
{
    surfer_pad_obj_t *o = MP_OBJ_TO_PTR(self_in);
    if (dest[0] != MP_OBJ_NULL)
        return;                       /* attributes are read-only */
    uint8_t d = surf_pad_dpad(o->idx);
    uint16_t b = surf_pad_buttons(o->idx);
    switch (attr) {
    case MP_QSTR_up:     dest[0] = mp_obj_new_bool(d & SURF_DPAD_UP); return;
    case MP_QSTR_down:   dest[0] = mp_obj_new_bool(d & SURF_DPAD_DOWN); return;
    case MP_QSTR_left:   dest[0] = mp_obj_new_bool(d & SURF_DPAD_LEFT); return;
    case MP_QSTR_right:  dest[0] = mp_obj_new_bool(d & SURF_DPAD_RIGHT); return;
    case MP_QSTR_a:      dest[0] = mp_obj_new_bool(b & SURF_BTN_A); return;
    case MP_QSTR_b:      dest[0] = mp_obj_new_bool(b & SURF_BTN_B); return;
    case MP_QSTR_x:      dest[0] = mp_obj_new_bool(b & SURF_BTN_X); return;
    case MP_QSTR_y:      dest[0] = mp_obj_new_bool(b & SURF_BTN_Y); return;
    case MP_QSTR_l:      dest[0] = mp_obj_new_bool(b & SURF_BTN_L); return;
    case MP_QSTR_r:      dest[0] = mp_obj_new_bool(b & SURF_BTN_R); return;
    case MP_QSTR_start:  dest[0] = mp_obj_new_bool(b & SURF_BTN_START); return;
    case MP_QSTR_select: dest[0] = mp_obj_new_bool(b & SURF_BTN_SELECT); return;
    case MP_QSTR_lx: dest[0] = mp_obj_new_float(pad_axf(o->idx, 0, 0)); return;
    case MP_QSTR_ly: dest[0] = mp_obj_new_float(pad_axf(o->idx, 0, 1)); return;
    case MP_QSTR_rx: dest[0] = mp_obj_new_float(pad_axf(o->idx, 1, 0)); return;
    case MP_QSTR_ry: dest[0] = mp_obj_new_float(pad_axf(o->idx, 1, 1)); return;
    case MP_QSTR_dpad:    dest[0] = MP_OBJ_NEW_SMALL_INT(d); return;
    case MP_QSTR_buttons: dest[0] = MP_OBJ_NEW_SMALL_INT(b); return;
    default: dest[1] = MP_OBJ_SENTINEL; return;   /* let methods resolve */
    }
}

/* pad.set_dpad(bits) — SURF_DPAD_* OR'd; replaces the whole hat */
static mp_obj_t pad_set_dpad(mp_obj_t self_in, mp_obj_t bits)
{
    surfer_pad_obj_t *o = MP_OBJ_TO_PTR(self_in);
    surf_pad_set_dpad(o->idx, (uint8_t)mp_obj_get_int(bits));
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_2(pad_set_dpad_obj, pad_set_dpad);

/* pad.set_buttons(bits) — SURF_BTN_* OR'd; replaces all buttons */
static mp_obj_t pad_set_buttons(mp_obj_t self_in, mp_obj_t bits)
{
    surfer_pad_obj_t *o = MP_OBJ_TO_PTR(self_in);
    surf_pad_set_buttons(o->idx, (uint16_t)mp_obj_get_int(bits));
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_2(pad_set_buttons_obj, pad_set_buttons);

/* pad.set_stick(stick, x, y) — floats in [-1, 1] */
static mp_obj_t pad_set_stick(size_t n, const mp_obj_t *a)
{
    surfer_pad_obj_t *o = MP_OBJ_TO_PTR(a[0]);
    int s = mp_obj_get_int(a[1]);
    surf_pad_set_axis(o->idx, s, 0, (int32_t)(mp_obj_get_float(a[2]) * SURF_ONE));
    surf_pad_set_axis(o->idx, s, 1, (int32_t)(mp_obj_get_float(a[3]) * SURF_ONE));
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(pad_set_stick_obj, 4, 4, pad_set_stick);

static mp_obj_t pad_reset(mp_obj_t self_in)
{
    surfer_pad_obj_t *o = MP_OBJ_TO_PTR(self_in);
    surf_pad_reset(o->idx);
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_1(pad_reset_obj, pad_reset);

static const mp_rom_map_elem_t pad_locals_table[] = {
    {MP_ROM_QSTR(MP_QSTR_set_dpad), MP_ROM_PTR(&pad_set_dpad_obj)},
    {MP_ROM_QSTR(MP_QSTR_set_buttons), MP_ROM_PTR(&pad_set_buttons_obj)},
    {MP_ROM_QSTR(MP_QSTR_set_stick), MP_ROM_PTR(&pad_set_stick_obj)},
    {MP_ROM_QSTR(MP_QSTR_reset), MP_ROM_PTR(&pad_reset_obj)},
};
static MP_DEFINE_CONST_DICT(pad_locals_dict, pad_locals_table);

MP_DEFINE_CONST_OBJ_TYPE(surfer_pad_type, MP_QSTR_Pad, MP_TYPE_FLAG_NONE,
                         attr, pad_attr, locals_dict, &pad_locals_dict);

/* surfer.pad(n=0) -> Pad handle for slot n (0..3). Cheap; make one and
 * read it each frame. */
static mp_obj_t mod_pad(size_t n, const mp_obj_t *args)
{
    int idx = n ? mp_obj_get_int(args[0]) : 0;
    if (idx < 0 || idx >= SURF_MAX_PADS)
        mp_raise_ValueError(MP_ERROR_TEXT("pad index out of range"));
    surfer_pad_obj_t *o = mp_obj_malloc(surfer_pad_obj_t, &surfer_pad_type);
    o->idx = idx;
    return MP_OBJ_FROM_PTR(o);
}
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(mod_pad_obj, 0, 1, mod_pad);

/* which pad slot the built-in keyboard map feeds each tick; -1 = off */
static int g_pad_keys = 0;

/* surfer.pad_keys(pad=0) — route the keyboard into a pad slot as a
 * source (arrows/WASD -> dpad, space/Z -> A, X -> B, C -> X, V -> Y,
 * Q -> L, E -> R). Pass -1 to turn the mapping off. Returns nothing. */
static mp_obj_t mod_pad_keys(size_t n, const mp_obj_t *args)
{
    int p = n ? mp_obj_get_int(args[0]) : 0;
    g_pad_keys = (p >= 0 && p < SURF_MAX_PADS) ? p : -1;
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(mod_pad_keys_obj, 0, 1, mod_pad_keys);

/* map the currently-held keys onto the keyboard pad slot (called each
 * tick from mod_tick). Keyboard is one source among many; a real
 * gamepad driver writes a different slot. */
static void pad_pump_keys(void)
{
    if (g_pad_keys < 0)
        return;
    surfer_key k[8];
    int n = surf_key_held(k, 8);
    uint8_t dpad = 0;
    uint16_t btn = 0;
    for (int i = 0; i < n; i++) {
        switch (k[i].kind) {
        case SURFER_KEY_LEFT:  dpad |= SURF_DPAD_LEFT; break;
        case SURFER_KEY_RIGHT: dpad |= SURF_DPAD_RIGHT; break;
        case SURFER_KEY_UP:    dpad |= SURF_DPAD_UP; break;
        case SURFER_KEY_DOWN:  dpad |= SURF_DPAD_DOWN; break;
        case SURFER_KEY_TEXT: {
            char c = k[i].utf8[0];
            if (c >= 'A' && c <= 'Z') c += 32;   /* fold case */
            switch (c) {
            case 'w': dpad |= SURF_DPAD_UP; break;
            case 's': dpad |= SURF_DPAD_DOWN; break;
            case 'a': dpad |= SURF_DPAD_LEFT; break;
            case 'd': dpad |= SURF_DPAD_RIGHT; break;
            case ' ': case 'z': btn |= SURF_BTN_A; break;
            case 'x': btn |= SURF_BTN_B; break;
            case 'c': btn |= SURF_BTN_X; break;
            case 'v': btn |= SURF_BTN_Y; break;
            case 'q': btn |= SURF_BTN_L; break;
            case 'e': btn |= SURF_BTN_R; break;
            }
            break;
        }
        }
    }
    /* source 1: the gamepad driver writes source 0, so a keyboard and a
     * gamepad merge and either drives the pad */
    surf_pad_set_dpad_src(g_pad_keys, 1, dpad);
    surf_pad_set_buttons_src(g_pad_keys, 1, btn);
}

/* surfer.has_touch() — did the touch controller come up? */
static mp_obj_t mod_has_touch(void)
{
    return mp_obj_new_bool(surfer_port_has_touch());
}
static MP_DEFINE_CONST_FUN_OBJ_0(mod_has_touch_obj, mod_has_touch);

/* surfer._touch_info() — controller's configured (x_max, y_max), or None */
static mp_obj_t mod_touch_info(void)
{
    int xm, ym;
    if (!surfer_port_touch_info(&xm, &ym))
        return mp_const_none;
    mp_obj_t t[2] = {MP_OBJ_NEW_SMALL_INT(xm), MP_OBJ_NEW_SMALL_INT(ym)};
    return mp_obj_new_tuple(2, t);
}
static MP_DEFINE_CONST_FUN_OBJ_0(mod_touch_info_obj, mod_touch_info);

static mp_obj_t mod_init(size_t n_args, const mp_obj_t *args)
{
    int16_t w = n_args > 0 ? (int16_t)mp_obj_get_int(args[0]) : 1024;
    int16_t h = n_args > 1 ? (int16_t)mp_obj_get_int(args[1]) : 600;
    /* p4 only, first init only: compose straight into the scan buffer —
     * the right mode for full-screen-every-frame animation */
    bool single = n_args > 2 && mp_obj_is_true(args[2]);
    /* 2048: real apps blow 512 fast — tulip5's drum machine alone holds
     * ~1100 live nodes (8 channel strips + a 155-row sound chooser).
     * Pool RAM is ~sizeof(surf_node)+paint per slot; at 2048 that is a
     * few hundred KB, fine on every backend. Exhaustion raises
     * RuntimeError mid-scene-build, which presents as a half-alive UI
     * (everything built before the throw works, nothing after does) —
     * found the hard way. */
    surf_config cfg = {.max_nodes = 2048, .bg = SURF_RGB(18, 20, 25)};
    if (inited) {
        /* soft reset (or repeat init): the VM dropped every Python object,
         * so rebuild the C scene from scratch on the surviving hal —
         * stale nodes with dangling callbacks must not outlive the VM.
         * The registry list died with the old heap; drop the root pointer
         * so registry_add rebuilds it instead of appending into freed
         * memory (store fault on the first node after Ctrl-D otherwise). */
        MP_STATE_VM(surfer_registry) = MP_OBJ_NULL;
        surf_deinit();
        g_scr_w = w;
        g_scr_h = h;
        if (!surf_init(g_hal, w, h, &cfg))
            mp_raise_msg(&mp_type_RuntimeError, MP_ERROR_TEXT("surf re-init failed"));
        return mp_const_none;
    }
    g_hal = surfer_port_init(w, h, single);
    g_scr_w = w;
    g_scr_h = h;
    if (!g_hal)
        mp_raise_msg(&mp_type_RuntimeError, MP_ERROR_TEXT("display init failed"));
    prepare_assets();
    if (!surf_init(g_hal, w, h, &cfg))
        mp_raise_msg(&mp_type_RuntimeError, MP_ERROR_TEXT("surf init failed"));
    inited = true;
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(mod_init_obj, 0, 3, mod_init);

static void pad_pump_keys(void);   /* keyboard -> pad, defined below */

static mp_obj_t mod_tick(void)
{
    if (!surfer_port_pump())
        return mp_const_false;
    pad_pump_keys();   /* keyboard -> pad slot (surfer.pad_keys) */
    surf_tick();
    return mp_const_true;
}
static MP_DEFINE_CONST_FUN_OBJ_0(mod_tick_obj, mod_tick);

static mp_obj_t mod_keys(void)
{
    mp_obj_t list = mp_obj_new_list(0, NULL);
    surfer_key k;
    while (surf_key_poll(&k)) {
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

/* surfer.keys_held() -> ((kind, text), ...): keys DOWN right now.
 * Events (keys()) are for typing; this is for games — poll it per
 * frame and move + fire at once. */
static mp_obj_t mod_keys_held(void)
{
    surfer_key k[8];
    int n = surf_key_held(k, 8);
    mp_obj_t items[8];
    for (int i = 0; i < n; i++) {
        mp_obj_t pair[2] = {
            MP_OBJ_NEW_SMALL_INT(k[i].kind),
            mp_obj_new_str(k[i].utf8, strlen(k[i].utf8)),
        };
        items[i] = mp_obj_new_tuple(2, pair);
    }
    return mp_obj_new_tuple((size_t)n, items);
}
static MP_DEFINE_CONST_FUN_OBJ_0(mod_keys_held_obj, mod_keys_held);

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

/* surfer.image(png_bytes, a8=False) -> Image. Read the file in Python —
 * bytes work the same on unix, the P4's VFS, and web MEMFS/frozen
 * assets. a8=True keeps only the alpha channel: the mask draws in
 * .tint (a one-entry palette, blended in hardware on the P4). */
static mp_obj_t mod_image(size_t n_args, const mp_obj_t *args)
{
    mp_buffer_info_t buf;
    mp_get_buffer_raise(args[0], &buf, MP_BUFFER_READ);
    bool a8 = n_args > 1 && mp_obj_is_true(args[1]);
    surf_image *img = a8 ? surf_image_from_png_a8(buf.buf, buf.len)
                         : surf_image_from_png(buf.buf, buf.len);
    if (!img)
        mp_raise_ValueError(MP_ERROR_TEXT("png decode failed"));
    surfer_image_obj_t *o = mp_obj_malloc(surfer_image_obj_t, &surfer_image_type);
    o->img = img;
    return MP_OBJ_FROM_PTR(o);
}
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(mod_image_obj, 1, 2, mod_image);

/* surfer.image_new(w, h, alpha=False) -> blank Image for load-time
 * composition (bake tile maps / parallax strips into one image) */
static mp_obj_t mod_image_new(size_t n_args, const mp_obj_t *args)
{
    /* 0/False = opaque 565, 1/True = ARGB, surfer.A8 = tintable mask */
    mp_int_t fmt = n_args > 2 ? mp_obj_get_int(args[2]) : 0;
    surf_image *img = surf_image_new((int16_t)mp_obj_get_int(args[0]),
                                     (int16_t)mp_obj_get_int(args[1]),
                                     (surf_format)fmt);
    if (!img)
        mp_raise_ValueError(MP_ERROR_TEXT("image_new failed"));
    surfer_image_obj_t *o = mp_obj_malloc(surfer_image_obj_t, &surfer_image_type);
    o->img = img;
    return MP_OBJ_FROM_PTR(o);
}
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(mod_image_new_obj, 2, 3, mod_image_new);

/* surfer.layer(image, x, y, view_w) -> wrap-scrolling strip Node;
 * n.set_offset(px), n.fast_scroll(True) for the streaming band path */
static mp_obj_t mod_layer(size_t n_args, const mp_obj_t *args)
{
    if (!mp_obj_is_type(args[0], &surfer_image_type))
        mp_raise_TypeError(MP_ERROR_TEXT("expected surfer Image"));
    surfer_image_obj_t *io = MP_OBJ_TO_PTR(args[0]);
    if (!io->img)
        mp_raise_ValueError(MP_ERROR_TEXT("image destroyed"));
    surfer_node_obj_t *o = new_node_obj(surf_layer_new(
        io->img, (int16_t)mp_obj_get_int(args[1]),
        (int16_t)mp_obj_get_int(args[2]), (int16_t)mp_obj_get_int(args[3])));
    o->img_ref = args[0];
    return MP_OBJ_FROM_PTR(o);
}
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(mod_layer_obj, 4, 4, mod_layer);

/* surfer.sprite(image, x, y) -> Node with .scale / .rot */
static mp_obj_t mod_sprite(mp_obj_t img_in, mp_obj_t x_in, mp_obj_t y_in)
{
    if (!mp_obj_is_type(img_in, &surfer_image_type))
        mp_raise_TypeError(MP_ERROR_TEXT("expected surfer Image"));
    surfer_image_obj_t *io = MP_OBJ_TO_PTR(img_in);
    if (!io->img)
        mp_raise_ValueError(MP_ERROR_TEXT("image destroyed"));
    surfer_node_obj_t *o = new_node_obj(surf_sprite_new(
        io->img, (int16_t)mp_obj_get_int(x_in), (int16_t)mp_obj_get_int(y_in)));
    o->img_ref = img_in;
    return MP_OBJ_FROM_PTR(o);
}
static MP_DEFINE_CONST_FUN_OBJ_3(mod_sprite_obj, mod_sprite);

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

/* ---- Font (runtime textgrid font) ---- */

static mp_obj_t font_destroy(mp_obj_t self_in)
{
    surfer_font_obj_t *o = MP_OBJ_TO_PTR(self_in);
    if (o->font) {
        surf_font_free(o->font);
        o->font = NULL;
    }
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_1(font_destroy_obj, font_destroy);

static const mp_rom_map_elem_t font_locals_table[] = {
    {MP_ROM_QSTR(MP_QSTR_destroy), MP_ROM_PTR(&font_destroy_obj)},
};
static MP_DEFINE_CONST_DICT(font_locals_dict, font_locals_table);

/* .cell_w (the 'M' advance) / .cell_h (line height) — the textgrid cell */
static void font_attr(mp_obj_t self_in, qstr attr, mp_obj_t *dest)
{
    surfer_font_obj_t *o = MP_OBJ_TO_PTR(self_in);
    if (dest[0] == MP_OBJ_NULL && o->font) {
        if (attr == MP_QSTR_cell_h) {
            dest[0] = MP_OBJ_NEW_SMALL_INT(surf_font_line_h(o->font));
            return;
        }
        if (attr == MP_QSTR_cell_w) {
            int adv = 0;
            for (int32_t i = 0; i < o->font->nglyphs; i++)
                if (o->font->glyphs[i].cp == 'M') adv = o->font->glyphs[i].adv;
            dest[0] = MP_OBJ_NEW_SMALL_INT(adv);
            return;
        }
    }
    if (dest[0] == MP_OBJ_NULL)
        dest[1] = MP_OBJ_SENTINEL;
}

MP_DEFINE_CONST_OBJ_TYPE(surfer_font_type, MP_QSTR_Font, MP_TYPE_FLAG_NONE,
                         attr, font_attr, locals_dict, &font_locals_dict);

/* surfer.font(blob_bytes) -> Font. blob is a fontbake .py FONT value
 * (the "SFN1" format). Pass it to surfer.textgrid(..., font=f) for a
 * custom console font. Held alive by the grid + a GC root. */
static mp_obj_t mod_font(mp_obj_t data_in)
{
    mp_buffer_info_t buf;
    mp_get_buffer_raise(data_in, &buf, MP_BUFFER_READ);
    surf_font *f = surf_font_from_blob(buf.buf, buf.len);
    if (!f)
        mp_raise_ValueError(MP_ERROR_TEXT("bad font blob"));
    surfer_port_prepare_image(&f->atlas);   /* device DMA coherence */
    surfer_font_obj_t *o = mp_obj_malloc(surfer_font_obj_t, &surfer_font_type);
    o->font = f;
    registry_add(MP_OBJ_FROM_PTR(o));
    return MP_OBJ_FROM_PTR(o);
}
static MP_DEFINE_CONST_FUN_OBJ_1(mod_font_obj, mod_font);

static mp_obj_t mod_textgrid(size_t n_args, const mp_obj_t *args)
{
    surf_color fg = n_args > 2 ? (surf_color)mp_obj_get_int(args[2])
                               : SURF_RGB(200, 205, 215);
    surf_color bg = n_args > 3 ? (surf_color)mp_obj_get_int(args[3])
                               : SURF_RGB(18, 20, 25);
    const surf_font *f;
    mp_obj_t font_ref = mp_const_none;
    if (n_args > 4 && mp_obj_is_type(args[4], &surfer_font_type)) {
        f = ((surfer_font_obj_t *)MP_OBJ_TO_PTR(args[4]))->font;
        font_ref = args[4];             /* anchor the runtime font */
    } else {
        f = font_of(n_args > 4 ? mp_obj_get_int(args[4]) : 2);
    }
    surfer_node_obj_t *o = new_node_obj(surf_textgrid_new(
        f, mp_obj_get_int(args[0]), mp_obj_get_int(args[1]), fg, bg));
    if (o)
        o->img_ref = font_ref;
    return MP_OBJ_FROM_PTR(o);
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

/* surfer.touches() -> ((id, x, y), ...) — the current multitouch
 * contacts, id-stable per finger. Poll each frame and diff by id; the
 * single-pointer on_touch dispatch is untouched (contact 0 drives it). */
static mp_obj_t mod_touches(void)
{
    surf_touch_pt pts[8];
    int n = surf_touch_points(pts, 8);
    mp_obj_t items[8];
    for (int i = 0; i < n; i++) {
        mp_obj_t t[3] = {MP_OBJ_NEW_SMALL_INT(pts[i].id),
                         MP_OBJ_NEW_SMALL_INT(pts[i].x),
                         MP_OBJ_NEW_SMALL_INT(pts[i].y)};
        items[i] = mp_obj_new_tuple(3, t);
    }
    return mp_obj_new_tuple((size_t)n, items);
}
static MP_DEFINE_CONST_FUN_OBJ_0(mod_touches_obj, mod_touches);

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
    {MP_ROM_QSTR(MP_QSTR_frame_rate), MP_ROM_PTR(&mod_frame_rate_obj)},
    {MP_ROM_QSTR(MP_QSTR_cpu), MP_ROM_PTR(&mod_cpu_obj)},
    {MP_ROM_QSTR(MP_QSTR_keys_held), MP_ROM_PTR(&mod_keys_held_obj)},
    {MP_ROM_QSTR(MP_QSTR_pad), MP_ROM_PTR(&mod_pad_obj)},
    {MP_ROM_QSTR(MP_QSTR_pad_keys), MP_ROM_PTR(&mod_pad_keys_obj)},
    {MP_ROM_QSTR(MP_QSTR_DPAD_UP), MP_ROM_INT(SURF_DPAD_UP)},
    {MP_ROM_QSTR(MP_QSTR_DPAD_DOWN), MP_ROM_INT(SURF_DPAD_DOWN)},
    {MP_ROM_QSTR(MP_QSTR_DPAD_LEFT), MP_ROM_INT(SURF_DPAD_LEFT)},
    {MP_ROM_QSTR(MP_QSTR_DPAD_RIGHT), MP_ROM_INT(SURF_DPAD_RIGHT)},
    {MP_ROM_QSTR(MP_QSTR_BTN_A), MP_ROM_INT(SURF_BTN_A)},
    {MP_ROM_QSTR(MP_QSTR_BTN_B), MP_ROM_INT(SURF_BTN_B)},
    {MP_ROM_QSTR(MP_QSTR_BTN_X), MP_ROM_INT(SURF_BTN_X)},
    {MP_ROM_QSTR(MP_QSTR_BTN_Y), MP_ROM_INT(SURF_BTN_Y)},
    {MP_ROM_QSTR(MP_QSTR_BTN_L), MP_ROM_INT(SURF_BTN_L)},
    {MP_ROM_QSTR(MP_QSTR_BTN_R), MP_ROM_INT(SURF_BTN_R)},
    {MP_ROM_QSTR(MP_QSTR_BTN_START), MP_ROM_INT(SURF_BTN_START)},
    {MP_ROM_QSTR(MP_QSTR_BTN_SELECT), MP_ROM_INT(SURF_BTN_SELECT)},
    {MP_ROM_QSTR(MP_QSTR_screen), MP_ROM_PTR(&mod_screen_obj)},
    {MP_ROM_QSTR(MP_QSTR_rgb), MP_ROM_PTR(&mod_rgb_obj)},
    {MP_ROM_QSTR(MP_QSTR_group), MP_ROM_PTR(&mod_group_obj)},
    {MP_ROM_QSTR(MP_QSTR_rect), MP_ROM_PTR(&mod_rect_obj)},
    {MP_ROM_QSTR(MP_QSTR_image), MP_ROM_PTR(&mod_image_obj)},
    {MP_ROM_QSTR(MP_QSTR_image_new), MP_ROM_PTR(&mod_image_new_obj)},
    {MP_ROM_QSTR(MP_QSTR_layer), MP_ROM_PTR(&mod_layer_obj)},
    {MP_ROM_QSTR(MP_QSTR_fb_read), MP_ROM_PTR(&mod_fb_read_obj)},
    {MP_ROM_QSTR(MP_QSTR_has_touch), MP_ROM_PTR(&mod_has_touch_obj)},
    {MP_ROM_QSTR(MP_QSTR__touch_info), MP_ROM_PTR(&mod_touch_info_obj)},
    {MP_ROM_QSTR(MP_QSTR_sprite), MP_ROM_PTR(&mod_sprite_obj)},
    {MP_ROM_QSTR(MP_QSTR_label), MP_ROM_PTR(&mod_label_obj)},
    {MP_ROM_QSTR(MP_QSTR_textgrid), MP_ROM_PTR(&mod_textgrid_obj)},
    {MP_ROM_QSTR(MP_QSTR_font), MP_ROM_PTR(&mod_font_obj)},
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
    {MP_ROM_QSTR(MP_QSTR_touches), MP_ROM_PTR(&mod_touches_obj)},
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
    {MP_ROM_QSTR(MP_QSTR_A8), MP_ROM_INT(SURF_FMT_A8)},
};
static MP_DEFINE_CONST_DICT(surfer_module_globals, surfer_globals_table);

const mp_obj_module_t surfer_user_cmodule = {
    .base = {&mp_type_module},
    .globals = (mp_obj_dict_t *)&surfer_module_globals,
};

MP_REGISTER_MODULE(MP_QSTR_surfer, surfer_user_cmodule);
