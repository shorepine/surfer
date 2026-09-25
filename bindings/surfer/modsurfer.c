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

/* ---- baked default style; pixels re-homed by surfer_port_prepare_image
 * at init (flash .rodata → PSRAM on device, no-op on desktop) ---- */

/* the knob and selector strips are NOT here: surf_art_knob_strip() and
 * surf_art_selector_strip() bake them on first use (src/widgets/art.c),
 * which is what took 565 KB off the device image. */
static surf_image track_img = {
    .pixels = (void *)widget_trackfull_px, .w = WTRACKFULL_W, .h = WTRACKFULL_H,
    .stride = WTRACKFULL_W * 4, .format = SURF_FMT_ARGB8888,
};
static surf_image cap_img = {
    .pixels = (void *)widget_cap_px, .w = WCAP_W, .h = WCAP_H,
    .stride = WCAP_W, .format = SURF_FMT_A8,
};
static surf_image radio_img = {
    .pixels = (void *)widget_radio_px, .w = WRADIO_SIZE * 2, .h = WRADIO_SIZE,
    .stride = WRADIO_SIZE * 2 * 4, .format = SURF_FMT_ARGB8888,
};
static surf_image check_img = {
    .pixels = (void *)widget_check_px, .w = WCHECK_SIZE * 2, .h = WCHECK_SIZE,
    .stride = WCHECK_SIZE * 2 * 4, .format = SURF_FMT_ARGB8888,
};
static surf_image sbar_img = {
    .pixels = (void *)widget_sbar_px, .w = WSBAR_W, .h = WSBAR_H,
    .stride = WSBAR_W * 4, .format = SURF_FMT_ARGB8888,
};
static surf_image sbtrack_img = {
    .pixels = (void *)widget_sbtrack_px, .w = WSBAR_W, .h = WSBAR_H,
    .stride = WSBAR_W * 4, .format = SURF_FMT_ARGB8888,
};
/* the same two capsules lying down, for horizontal bars */
static surf_image sbarh_img = {
    .pixels = (void *)widget_sbarh_px, .w = WSBAR_H, .h = WSBAR_W,
    .stride = WSBAR_H * 4, .format = SURF_FMT_ARGB8888,
};
static surf_image sbtrackh_img = {
    .pixels = (void *)widget_sbtrackh_px, .w = WSBAR_H, .h = WSBAR_W,
    .stride = WSBAR_H * 4, .format = SURF_FMT_ARGB8888,
};
static const surf_image led_img = {
    .pixels = (void *)widget_led_px, .w = WLED_SIZE * WLED_FRAMES,
    .h = WLED_SIZE, .stride = WLED_SIZE * WLED_FRAMES, .format = SURF_FMT_A8,
};
/* the slider's art lying down, for a horizontal one */
static surf_image trackh_img = {
    .pixels = (void *)widget_trackh_px, .w = WTRACK_SIZE, .h = WTRACK_SIZE,
    .stride = WTRACK_SIZE * 4, .format = SURF_FMT_ARGB8888,
};
static surf_image caph_img = {
    .pixels = (void *)widget_caph_px, .w = WCAP_H, .h = WCAP_W,
    .stride = WCAP_H, .format = SURF_FMT_A8,
};
/* the compact slider: a thin bar, a small handle wider than it */
static surf_image slimtrack_img = {
    .pixels = (void *)widget_slimtrack_px, .w = WSLIMTRACK_W, .h = WSLIMTRACK_H,
    .stride = WSLIMTRACK_W * 4, .format = SURF_FMT_ARGB8888,
};
static surf_image slimcap_img = {
    .pixels = (void *)widget_slimcap_px, .w = WSLIMCAP_W, .h = WSLIMCAP_H,
    .stride = WSLIMCAP_W, .format = SURF_FMT_A8,
};
static surf_image slimtrackh_img = {
    .pixels = (void *)widget_slimtrackh_px, .w = WSLIMTRACK_H, .h = WSLIMTRACK_W,
    .stride = WSLIMTRACK_H * 4, .format = SURF_FMT_ARGB8888,
};
static surf_image slimcaph_img = {
    .pixels = (void *)widget_slimcaph_px, .w = WSLIMCAP_H, .h = WSLIMCAP_W,
    .stride = WSLIMCAP_H, .format = SURF_FMT_A8,
};
static surf_image panel_img = {
    .pixels = (void *)widget_panel_px, .w = WPANEL_SIZE, .h = WPANEL_SIZE,
    .stride = WPANEL_SIZE * 4, .format = SURF_FMT_ARGB8888,
};
static surf_image btn_img = {
    .pixels = (void *)widget_btn_px, .w = WBTN_SIZE, .h = WBTN_SIZE,
    .stride = WBTN_SIZE * 4, .format = SURF_FMT_ARGB8888,
};
/* the tab's own 9-patch: A8, rounded at the top and flat at the foot, so
 * a tab joins the page under it. Const like the LED's — every tabs
 * widget copies the struct to hold its own tint. */
static const surf_image tab_img = {
    .pixels = (void *)widget_tab_px, .w = WTAB_W, .h = WTAB_H,
    .stride = WTAB_W, .format = SURF_FMT_A8,
};
static surf_image btnpr_img = {
    .pixels = (void *)widget_btnpr_px, .w = WBTN_SIZE, .h = WBTN_SIZE,
    .stride = WBTN_SIZE * 4, .format = SURF_FMT_ARGB8888,
};
static surf_image arrow_img = {
    .pixels = (void *)widget_arrow_px, .w = WARROW_W * 2, .h = WARROW_H,
    .stride = WARROW_W * 2 * 4, .format = SURF_FMT_ARGB8888,
};


static void prepare_assets(void)
{
    surfer_port_prepare_image(&track_img);
    surfer_port_prepare_image(&cap_img);
    surfer_port_prepare_image(&sbar_img);
    surfer_port_prepare_image(&sbtrack_img);
    surfer_port_prepare_image(&sbarh_img);
    surfer_port_prepare_image(&sbtrackh_img);
    surfer_port_prepare_image(&trackh_img);
    surfer_port_prepare_image(&caph_img);
    surfer_port_prepare_image(&slimtrack_img);
    surfer_port_prepare_image(&slimcap_img);
    surfer_port_prepare_image(&slimtrackh_img);
    surfer_port_prepare_image(&slimcaph_img);
    surfer_port_prepare_image(&check_img);
    surfer_port_prepare_image(&radio_img);
    surfer_port_prepare_image(&panel_img);
    surfer_port_prepare_image(&arrow_img);
    surfer_port_prepare_image(&btn_img);
    surfer_port_prepare_image(&btnpr_img);
    /* led_img stays const: every LED copies the struct and re-homing a
     * shared const would be re-homing it once per copy anyway. Its pixels
     * are 1 byte deep and small, so the P4 blends them from flash. */
    /* every built-in atlas, re-homed once into DMA-able RAM */
    surf_font_builtin_prepare(surfer_port_prepare_image);
}

/* ---- object types ---- */

static mp_obj_t hitboxes_obj_new(mp_obj_t node_ref);
static void hb_dbg_sync(void *node_obj);
static void hb_dbg_teardown(void *node_obj);

typedef struct {
    mp_obj_base_t base;
    surf_node *node;
    mp_obj_t touch_cb;  /* node.on_touch: fn(phase, x, y) or None */
    mp_obj_t img_ref;   /* sprites: keeps the Image object alive */
    mp_obj_t hb_dbg;    /* hitbox debug outlines: None, or a list of
                           per-index entries [group_wrapper|None, color] --
                           binding state, because C has no drawing to do
                           for a box that is not being debugged */
    bool     is_input;  /* textinput: taps place the caret (see ti_touch) */
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
    surf_mesh *m;       /* NULL after destroy() */
} surfer_mesh_obj_t;
extern const mp_obj_type_t surfer_mesh_type;

typedef struct {
    mp_obj_base_t base;
    surf_font *font;    /* NULL after destroy() */
    bool owned;         /* false for built-ins: shared, never freed */
} surfer_font_obj_t;
extern const mp_obj_type_t surfer_font_type;

enum { W_SLIDER, W_KNOB, W_CHECKBOX, W_DROPDOWN, W_BUTTON, W_SCROLLBAR,
       W_LED, W_SELECTOR, W_COLORPICKER, W_TABS, W_RADIO };

typedef struct {
    mp_obj_base_t base;
    uint8_t kind;
    void *w;          /* surf_slider* / surf_knob* / ... */
    surf_node *node;  /* widget root, for tree/pos ops */
    mp_obj_t callback;
    /* the knob's SECOND callback: a tap rather than a drag. Its own slot
     * because it reports something else — where the user pointed, not
     * what the value became. */
    mp_obj_t tap_cb;
    mp_obj_t node_ref; /* .node, made once — see new_node_obj */
} surfer_widget_obj_t;

extern const mp_obj_type_t surfer_node_type;
extern const mp_obj_type_t surfer_widget_type;
extern const mp_obj_type_t surfer_image_type;

/* Things Python creates stay reachable from here so the GC can't collect
 * an object the C side still points at (callbacks, tree links).
 *
 * ONE table, indexed by POOL SLOT, holding the one wrapper for the node
 * in that slot; surf_set_node_freed_cb clears the entry when the node
 * dies, children included. A node's own Python refs (its callback, a
 * runtime Font or Image it draws from) hang off that wrapper, so they
 * live exactly as long as the node does.
 *
 * It used to be an append-only list, and nothing was ever removed: every
 * node object Python ever created stayed rooted for the life of the
 * session. A task bar rebuilt on each colour-picker callback leaked
 * ~4.4 KB an event and died with `memory allocation failed`. The same
 * append also left a destroyed node's wrapper pointing at a pool slot
 * already handed to someone else.
 *
 * That list survived as `surfer_pins` for things with no node to hang
 * off, and it had to go too -- it was the one root a caller could write
 * through BEFORE surfer.init(). mp_init() does not clear a usermod's
 * root pointers (it clears its own, one subsystem at a time, in
 * runtime.c), so after a soft reset this one still pointed at a list in
 * a heap gc_init() had handed back. A stale pointer is not MP_OBJ_NULL,
 * so the guard did not fire and the append wrote through it: a store
 * fault, i.e. a board that does not come back. Reached from
 * widget_font(), which a host may legitimately call before init -- that
 * exact order killed the P4X 5/5, and it was as old as the registry
 * rather than anything a recent commit broke.
 *
 * Deleting it is safe because it was already protecting nothing. A Font
 * a NODE draws from is anchored by that node's own img_ref; a Font
 * nobody uses can be collected without harm, since surfer_font_type has
 * no finaliser and surf_font_free runs only from an explicit .destroy()
 * -- so a collected wrapper leaves the C font allocated rather than
 * leaving a pointer dangling. The pin leaked that same allocation, only
 * permanently. `surfer_nodes` stays: nothing reachable before init
 * touches it (registry_get/set both need a live scene), and mod_init's
 * re-init branch drops it. */
MP_REGISTER_ROOT_POINTER(mp_obj_t surfer_nodes);

/* The node pool, and so the length of the slot table. See mod_init for
 * why it is this number and what it costs. */
#define SURF_POOL_NODES 4096

static void on_node_freed(surf_node *n, int index);

/* One slot per pool node, all None. Built at init and after a soft reset,
 * which is the only time the pool is re-made. */
static void registry_init(void)
{
    mp_obj_t list = mp_obj_new_list(SURF_POOL_NODES, NULL);
    size_t len;
    mp_obj_t *items;
    mp_obj_list_get(list, &len, &items);
    for (size_t i = 0; i < len; i++)
        items[i] = mp_const_none;
    MP_STATE_VM(surfer_nodes) = list;
    surf_set_node_freed_cb(on_node_freed);
}

/* The slot table as a raw array: O(1), no allocation and nothing that can
 * raise, which matters because the freed callback runs inside destroy. */
static mp_obj_t *nodes_table(size_t *len)
{
    if (MP_STATE_VM(surfer_nodes) == MP_OBJ_NULL) {
        *len = 0;
        return NULL;
    }
    mp_obj_t *items;
    mp_obj_list_get(MP_STATE_VM(surfer_nodes), len, &items);
    return items;
}

static mp_obj_t registry_get(const surf_node *n)
{
    size_t len;
    mp_obj_t *items = nodes_table(&len);
    int i = surf_node_index(n);
    if (!items || i < 0 || (size_t)i >= len)
        return mp_const_none;
    return items[i];
}

static void registry_set(const surf_node *n, mp_obj_t o)
{
    size_t len;
    mp_obj_t *items = nodes_table(&len);
    int i = surf_node_index(n);
    if (items && i >= 0 && (size_t)i < len)
        items[i] = o;
}

/* The wrapper alone, claiming no slot. Only for a WIDGET's .node, whose
 * slot the widget object already owns — the widget roots this through
 * its own node_ref and on_node_freed blanks it from there. */
static surfer_node_obj_t *new_node_obj_raw(surf_node *n)
{
    if (!n)
        mp_raise_msg(&mp_type_RuntimeError, MP_ERROR_TEXT("node pool exhausted"));
    surfer_node_obj_t *o = mp_obj_malloc(surfer_node_obj_t, &surfer_node_type);
    o->node = n;
    o->touch_cb = mp_const_none;
    o->img_ref = mp_const_none;
    o->hb_dbg = mp_const_none;
    o->is_input = false;
    return o;
}

/* One wrapper per node, kept in the node's own slot. surfer.screen() used
 * to mint a fresh one per call — a leak on its own, and with a slot table
 * it would unroot the previous wrapper while the C node still pointed at
 * it through touch_user. Identity is now stable: screen() is screen(). */
static surfer_node_obj_t *new_node_obj(surf_node *n)
{
    mp_obj_t have = registry_get(n);
    if (have != mp_const_none && have != MP_OBJ_NULL &&
        mp_obj_get_type(have) == &surfer_node_type)
        return MP_OBJ_TO_PTR(have);
    surfer_node_obj_t *o = new_node_obj_raw(n);
    registry_set(n, MP_OBJ_FROM_PTR(o));
    return o;
}

/* Every node the library frees comes through here, so this is where a
 * wrapper stops being rooted and stops pointing at a recycled slot. */
static void on_node_freed(surf_node *n, int index)
{
    (void)n;
    size_t len;
    mp_obj_t *items = nodes_table(&len);
    if (!items || index < 0 || (size_t)index >= len)
        return;
    mp_obj_t o = items[index];
    items[index] = mp_const_none;
    if (o == mp_const_none || o == MP_OBJ_NULL)
        return;
    const mp_obj_type_t *t = mp_obj_get_type(o);
    if (t == &surfer_node_type) {
        ((surfer_node_obj_t *)MP_OBJ_TO_PTR(o))->node = NULL;
    } else if (t == &surfer_widget_type) {
        surfer_widget_obj_t *w = MP_OBJ_TO_PTR(o);
        w->node = NULL;
        w->w = NULL;
        /* .node handed out a wrapper of its own; blank that too */
        if (w->node_ref != mp_const_none && w->node_ref != MP_OBJ_NULL &&
            mp_obj_get_type(w->node_ref) == &surfer_node_type)
            ((surfer_node_obj_t *)MP_OBJ_TO_PTR(w->node_ref))->node = NULL;
    }
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
    const surf_font *f = surf_font_builtin_at((int)i);
    if (!f)
        mp_raise_ValueError(MP_ERROR_TEXT("bad font"));
    return f;
}

/* The two faces this binding picks when the caller names none.
 *
 * Both go through a NAME, never surf_font_builtin_at(0): index 0 is
 * merely whatever comes first in the Makefile's font list, so reordering
 * that list silently restyled every default label and every widget.
 *
 * WIDGET_FONT is what button and dropdown labels draw with, and it is
 * `ui12` — the same face `surfer.label` defaults to, so chrome matches
 * the text beside it. It used to be a drawn bitmap (helvR08) because a
 * thresholded outline smears at chrome sizes; that was true right up
 * until fontbake started sizing in ppem and hinting through FreeType,
 * which is what made small antialiased text hold together at all.
 *
 * `surfer.widget_font(name_or_font)` overrides it for widgets made AFTER
 * the call — a button bakes its label node at construction, so this is
 * naturally "from here on" rather than retroactive. */
#define DEFAULT_FONT "ui12"
#define WIDGET_FONT  "ui12"

/* The registry hands back the prepared copy, so this is safe on backends
 * whose blitter can't read the atlas where the linker put it. The
 * fallback covers a build that trims the named face out of the registry
 * — a NULL style font would draw nothing at all. */
static const surf_font *font_named(const char *name)
{
    const surf_font *f = surf_font_builtin(name);
    return f ? f : surf_font_builtin_at(0);
}

/* Resolve whatever the caller passed as a font: a Font object (runtime
 * blob or a named built-in), a name string ("helvR12"), or a legacy
 * index. Sets *ref to the object that must stay alive for the node. */
/* What widget chrome draws with. Resolved lazily, because the registry
 * is not up yet when this file's statics are initialised.
 *
 * The name is a COPY rather than the mp_obj_t the caller passed. These
 * are C statics: they outlive the VM that set them, so holding a Python
 * object here means widget_font() can hand back one the GC freed a soft
 * reset ago. The cached surf_font* is fine to keep either way -- a
 * built-in is static data, and a runtime one is only freed by an
 * explicit Font.destroy(). Empty name means a runtime Font, which has no
 * name to report. */
static const surf_font *widget_font_cache;
static char widget_font_name[32];   /* "" until a host names one */
static bool widget_font_unnamed;    /* last set from a Font object */

static const surf_font *font_arg(mp_obj_t o, mp_obj_t *ref);

static const surf_font *widget_font(void)
{
    if (!widget_font_cache)
        widget_font_cache = font_named(WIDGET_FONT);
    return widget_font_cache;
}

static const surf_font *font_arg(mp_obj_t o, mp_obj_t *ref)
{
    if (ref)
        *ref = mp_const_none;
    if (mp_obj_is_type(o, &surfer_font_type)) {
        surfer_font_obj_t *fo = MP_OBJ_TO_PTR(o);
        if (!fo->font)
            mp_raise_ValueError(MP_ERROR_TEXT("font destroyed"));
        if (ref)
            *ref = o;                    /* anchor: node outlives the call */
        return fo->font;
    }
    if (mp_obj_is_str(o)) {
        const surf_font *f = surf_font_builtin(mp_obj_str_get_str(o));
        if (!f)
            mp_raise_ValueError(MP_ERROR_TEXT("no such font"));
        return f;
    }
    return font_of(mp_obj_get_int(o));
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
    hb_dbg_teardown(o);
    surf_node_destroy(o->node);
    o->node = NULL;
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_1(node_destroy_obj, node_destroy);

/* filmstrip transport. stop() freezes the strip keeping its fps;
 * play() resumes at that speed, and play(fps) sets the speed first —
 * the answer to "I set fps to 0 and then 10", which works too but
 * conflates the speed with the transport. Both guard on the node type
 * in C, so they are harmless no-ops on anything else. */
static mp_obj_t node_play(size_t n_args, const mp_obj_t *args)
{
    surf_node *n = node_of(args[0]);
    if (n_args == 2) {
        mp_float_t f = mp_obj_get_float(args[1]);
        if (f < 0)
            f = 0;
        surf_filmstrip_set_fps(n, (int32_t)(f * (mp_float_t)SURF_ONE));
    }
    surf_filmstrip_set_playing(n, true);
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(node_play_obj, 1, 2, node_play);

static mp_obj_t node_stop(mp_obj_t self_in)
{
    surf_filmstrip_set_playing(node_of(self_in), false);
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_1(node_stop_obj, node_stop);

static mp_obj_t node_set_text(mp_obj_t self_in, mp_obj_t s)
{
    surfer_node_obj_t *o = MP_OBJ_TO_PTR(self_in);
    const char *str = mp_obj_str_get_str(s);
    /* both node types spell it set_text here; the C calls are separate
     * and each ignores the other's node, so without this a set_text on a
     * text field would silently do nothing */
    if (o->is_input)
        surf_textinput_set_text(node_of(self_in), str);
    else
        surf_text_set(node_of(self_in), str);
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

/* UTF-8 advance from the lead byte; a stray continuation counts as one so
 * a malformed string still terminates. */
static int u8_len(unsigned char b)
{
    if (b < 0x80) return 1;
    if ((b & 0xe0) == 0xc0) return 2;
    if ((b & 0xf0) == 0xe0) return 3;
    if ((b & 0xf8) == 0xf0) return 4;
    return 1;
}

/* grid.set_cells(col, row, s, fg, bg) — a RUN of cells in ONE call.
 *
 * set_cell is per character, so a terminal repainting a screen of
 * coloured text pays a MicroPython call per cell plus the interpreter
 * loop driving it: measured on tulip5's editor, 2244 cells cost 19 ms of
 * a 112 ms page on the P4X, and the Python around it cost more again.
 * This is that loop, moved down. Same clipping and the same per-cell
 * early-out as set_cell, so it damages exactly what changed. */
static mp_obj_t node_set_cells(size_t n_args, const mp_obj_t *args)
{
    surf_node *g = node_of(args[0]);
    int16_t col = (int16_t)mp_obj_get_int(args[1]);
    int16_t row = (int16_t)mp_obj_get_int(args[2]);
    const char *s = mp_obj_str_get_str(args[3]);
    surf_color fg = n_args > 4 ? (surf_color)mp_obj_get_int(args[4]) : 0xffff;
    surf_color bg = n_args > 5 ? (surf_color)mp_obj_get_int(args[5]) : 0x0000;
    while (*s) {
        surf_textgrid_set_cell(g, col, row, surf_utf8_first(s), fg, bg);
        s += u8_len((unsigned char)*s);
        col++;
    }
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(node_set_cells_obj, 4, 6,
                                           node_set_cells);

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

/* sprite.set_image(img) / filmstrip.set_image(img) — show a DIFFERENT
 * picture on the node that is already there.
 *
 * The binding's own half of this is `img_ref`, and it is the whole
 * reason this cannot be a one-liner over the core call: a node object
 * anchors the Image object it draws from, so a swap that moved the C
 * pointer and left the reference behind would root the picture nobody
 * is drawing and leave the new one collectable — and an Image collected
 * out from under a live sprite is pixels freed while the compositor
 * still blits them, which on this machine is an exit with status 0 and
 * no traceback anywhere. Rebound after the core call takes it, never
 * before: a refusal must not repoint the anchor.
 *
 * It RAISES rather than returning False, because every reason it can
 * fail is a mistake in the calling line — a label instead of a sprite,
 * a strip narrower than the frame size it was built with — and a
 * silently unchanged picture is the failure mode surf_sprite_set_xform
 * spent a release having (the code looks right and the picture never
 * moves). */
static mp_obj_t node_set_image(mp_obj_t self_in, mp_obj_t img_in)
{
    if (!mp_obj_is_type(img_in, &surfer_image_type))
        mp_raise_TypeError(MP_ERROR_TEXT("expected surfer Image"));
    surfer_image_obj_t *io = MP_OBJ_TO_PTR(img_in);
    if (!io->img)
        mp_raise_ValueError(MP_ERROR_TEXT("image destroyed"));
    surfer_node_obj_t *o = MP_OBJ_TO_PTR(self_in);
    if (!surf_sprite_set_image(node_of(self_in), io->img))
        mp_raise_ValueError(MP_ERROR_TEXT(
            "set_image wants a sprite or a filmstrip big enough for its frame"));
    o->img_ref = img_in;
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_2(node_set_image_obj, node_set_image);

/* layer.set_offset(px) — float pixels; wraps at the strip width */
static mp_obj_t node_set_offset(mp_obj_t self_in, mp_obj_t off)
{
    surf_layer_set_offset(node_of(self_in),
                          (int32_t)(mp_obj_get_float(off) * SURF_ONE));
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_2(node_set_offset_obj, node_set_offset);

/* grid.scrollback(mult) -> bool: keep mult screens of history and let a
 * drag look back through it. See surf_textgrid_set_scrollback. */
static mp_obj_t node_scrollback(mp_obj_t self_in, mp_obj_t mult_in)
{
    return mp_obj_new_bool(surf_textgrid_set_scrollback(
        node_of(self_in), (int16_t)mp_obj_get_int(mult_in)));
}
static MP_DEFINE_CONST_FUN_OBJ_2(node_scrollback_obj, node_scrollback);

/* grid.view() -> rows scrolled back; grid.view(n) to set it. 0 = live. */
static mp_obj_t node_view(size_t n_args, const mp_obj_t *args)
{
    surf_node *n = node_of(args[0]);
    if (n_args > 1)
        surf_textgrid_set_view(n, (int16_t)mp_obj_get_int(args[1]));
    return MP_OBJ_NEW_SMALL_INT(surf_textgrid_view(n));
}
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(node_view_obj, 1, 2, node_view);

/* grid.history() -> rows of scrollback above the current view */
static mp_obj_t node_history(mp_obj_t self_in)
{
    return MP_OBJ_NEW_SMALL_INT(surf_textgrid_history(node_of(self_in)));
}
static MP_DEFINE_CONST_FUN_OBJ_1(node_history_obj, node_history);

/* grid.set_colors(fg, bg): recolour a live textgrid. */
static mp_obj_t node_set_colors(mp_obj_t self_in, mp_obj_t fg, mp_obj_t bg)
{
    surf_textgrid_set_colors(node_of(self_in), (surf_color)mp_obj_get_int(fg),
                             (surf_color)mp_obj_get_int(bg));
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_3(node_set_colors_obj, node_set_colors);

/* g.set_clip(w, h): give a GROUP a size — which is also what makes it
 * hittable, and what lets a handler on it stand for everything inside.
 * 0x0 turns clipping off again. */
static mp_obj_t node_set_clip(mp_obj_t self_in, mp_obj_t w, mp_obj_t h)
{
    surf_group_set_clip(node_of(self_in), (int16_t)mp_obj_get_int(w),
                        (int16_t)mp_obj_get_int(h));
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_3(node_set_clip_obj, node_set_clip);

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

/* a.hits(b) — do the two nodes overlap ON THE INK? Boxes first, then a
 * sprite/filmstrip answers from its image's alpha, so transparent
 * corners do not collide. See surf_node_overlaps in surfer.h. */
static mp_obj_t node_hits(mp_obj_t self_in, mp_obj_t other_in)
{
    surf_node *a = node_of(self_in), *b = node_of(other_in);
    /* With hitboxes the answer is WHICH ones: a tuple of indices, empty
     * when nothing hit -- truthy exactly when a bool would have been, so
     * every `if a.hits(b):` ever written keeps working. Without them the
     * bool it has always been. */
    if (surf_hitbox_count(a)) {
        uint32_t m = surf_node_overlaps_which(a, b);
        mp_obj_t items[SURF_MAX_HITBOXES];
        size_t k = 0;
        for (int32_t i = 0; i < SURF_MAX_HITBOXES; i++)
            if (m & (1u << i))
                items[k++] = MP_OBJ_NEW_SMALL_INT(i);
        return mp_obj_new_tuple(k, items);
    }
    return mp_obj_new_bool(surf_node_overlaps(a, b));
}
static MP_DEFINE_CONST_FUN_OBJ_2(node_hits_obj, node_hits);

/* ---- textinput methods (safe no-ops on any other node: every
   surf_textinput_* entry point guards on the node type) ---- */

static mp_obj_t node_insert(mp_obj_t self_in, mp_obj_t s)
{
    surf_textinput_insert(node_of(self_in), mp_obj_str_get_str(s));
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_2(node_insert_obj, node_insert);

static mp_obj_t node_backspace(mp_obj_t self_in)
{
    surf_textinput_backspace(node_of(self_in));
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_1(node_backspace_obj, node_backspace);

static mp_obj_t node_delete(mp_obj_t self_in)
{
    surf_textinput_delete(node_of(self_in));
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_1(node_delete_obj, node_delete);

/* move(delta_codepoints, extend=False) — big deltas are home/end */
static mp_obj_t node_move(size_t n_args, const mp_obj_t *args)
{
    surf_textinput_move(node_of(args[0]), mp_obj_get_int(args[1]),
                        n_args > 2 && mp_obj_is_true(args[2]));
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(node_move_obj, 2, 3, node_move);

static mp_obj_t node_focus(size_t n_args, const mp_obj_t *args)
{
    surf_textinput_set_focused(node_of(args[0]),
                               n_args < 2 || mp_obj_is_true(args[1]));
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(node_focus_obj, 1, 2, node_focus);

static mp_obj_t node_index_from_x(mp_obj_t self_in, mp_obj_t x)
{
    return MP_OBJ_NEW_SMALL_INT(
        surf_textinput_index_from_x(node_of(self_in),
                                    (int16_t)mp_obj_get_int(x)));
}
static MP_DEFINE_CONST_FUN_OBJ_2(node_index_from_x_obj, node_index_from_x);

/* key(k): apply ONE event from surfer.keys() — the (kind, text, shift,
 * ctrl) tuple — and say whether it was consumed. Editing is a fixed
 * mapping from key to edit, so it lives here rather than being retyped
 * as a dispatch table in every app.
 *
 * ctrl is READ OFF THE TUPLE AND IGNORED, deliberately: a textinput is
 * one line with no word motion and no delete-word, so every chord means
 * what the bare key means. It is unpacked anyway so that a tuple of
 * either length works — this takes `len < 2` and looks no further.
 *
 * A CONSUMER MUST NOT ASSUME THE LENGTH. It was 3 before ctrl existed
 * and is 4 now, which is exactly the breakage a `kind, text, shift = k`
 * takes; index, or unpack with the full width.
 *
 *     for k in surfer.keys():
 *         if not ti.key(k):
 *             ...                      # not ours: hotkeys, Enter, ...
 */
static mp_obj_t node_key(mp_obj_t self_in, mp_obj_t k)
{
    surf_node *n = node_of(self_in);
    size_t len;
    mp_obj_t *item;
    mp_obj_get_array(k, &len, &item);
    if (len < 2)
        mp_raise_ValueError(MP_ERROR_TEXT("not a key event"));
    int kind = mp_obj_get_int(item[0]);
    bool shift = len > 2 && mp_obj_is_true(item[2]);
    switch (kind) {
    case SURFER_KEY_TEXT: {
        /* TAB IS NAVIGATION, NOT TEXT. The hal pushes it as TEXT 0x09
         * (there is no KEY_TAB), so inserting it put an invisible tab
         * character into whatever field had the keys -- a wifi password
         * that then never matched, typed by a host that only wanted to
         * move to the next field. Refused like Enter in a one-line
         * field: False, "not mine", and the host moves focus. */
        const char *t = mp_obj_str_get_str(item[1]);
        if (t[0] == '\t' && t[1] == 0)
            return mp_const_false;
        surf_textinput_insert(n, t);
        break;
    }
    case SURFER_KEY_LEFT:      surf_textinput_move(n, -1, shift); break;
    case SURFER_KEY_RIGHT:     surf_textinput_move(n, 1, shift); break;
    case SURFER_KEY_HOME:      surf_textinput_move(n, -99999, shift); break;
    case SURFER_KEY_END:       surf_textinput_move(n, 99999, shift); break;
    case SURFER_KEY_BACKSPACE: surf_textinput_backspace(n); break;
    case SURFER_KEY_DELETE:    surf_textinput_delete(n); break;
    case SURFER_KEY_ENTER:
        /* A NEW LINE IN A MULTILINE FIELD, AND THE CALLER'S OTHERWISE.
         * A one-line field's Enter has always meant `submit` — every
         * host here binds it — so taking it would break every one of
         * them; in a textarea it can only mean a line break, and a
         * caller that had to special-case that would be writing the one
         * thing the widget is for. False still means "not handled",
         * which is how the single-line answer stays exactly as it was. */
        if (!surf_textinput_rows(n))
            return mp_const_false;
        surf_textinput_insert(n, "\n");
        break;
    case SURFER_KEY_UP:
    case SURFER_KEY_DOWN: {
        /* UP AND DOWN ARE A LINE, and only a wrapped field can say what
         * a line is: the caret's own x carried to the row above or
         * below, through the same layout the glyphs were painted with.
         * A single-line field has nowhere to go and says so. */
        if (!surf_textinput_rows(n))
            return mp_const_false;
        surf_textinput_move_line(n, kind == SURFER_KEY_UP ? -1 : 1, shift);
        break;
    }
    default:
        return mp_const_false;   /* PgUp/Dn and the rest: yours */
    }
    return mp_const_true;
}
static MP_DEFINE_CONST_FUN_OBJ_2(node_key_obj, node_key);

/* spr.tween("x_pos", 300, 1000, ease="out")
 *
 * THE PROPERTY IS A NAME, and it has to be. `spr.tween(spr.x_pos, ...)`
 * is the obvious spelling and cannot work: `spr.x_pos` evaluates to an
 * INTEGER before tween is called, and Python has no way to hand over
 * the slot it came out of. A string is what QPropertyAnimation and
 * Cocoa's keyPath take, for exactly this reason.
 *
 * `to` is in the property's OWN units — pixels for x_pos/y_pos, 0..1 for
 * opacity, a multiplier for scale — and the Q16 the C side wants is
 * this function's business, not the caller's. `start` overrides where it
 * begins; by default it is wherever the property is now.
 *
 * There is no "rot": the PPA turns in quarter turns, so a smooth rotate
 * is not a thing this hardware can do and pretending otherwise would
 * give a four-frame flip-book. Naming it raises with that sentence.
 */
static const char *const TW_NAMES[] = {"opacity", "x_pos", "y_pos", "scale"};

static int tw_prop(mp_obj_t o)
{
    const char *want = mp_obj_str_get_str(o);
    for (int i = 0; i < SURF_TW_NPROPS; i++)
        if (!strcmp(want, TW_NAMES[i]))
            return i;
    if (!strcmp(want, "rot"))
        mp_raise_ValueError(MP_ERROR_TEXT(
            "rot cannot tween - the PPA turns in quarter turns only, so a "
            "smooth rotate would be four frames. Set .rot directly"));
    mp_raise_ValueError(MP_ERROR_TEXT(
        "tween takes 'opacity', 'x_pos', 'y_pos' or 'scale'"));
    return 0;
}

/* the property's own unit <-> the Q16 the tween interpolates */
static int32_t tw_to_q16(int prop, mp_obj_t v)
{
    if (prop == SURF_TW_OPACITY) {
        mp_float_t f = mp_obj_get_float(v);
        if (f < 0) f = 0;
        if (f > 1) f = 1;
        return (int32_t)(f * 255 * 65536 + (mp_float_t)0.5);
    }
    if (prop == SURF_TW_SCALE)
        return (int32_t)(mp_obj_get_float(v) * SURF_ONE);
    return (int32_t)(mp_obj_get_int(v)) << 16;   /* x_pos / y_pos: pixels */
}

static uint8_t tw_ease(mp_obj_t o)
{
    if (o == MP_OBJ_NULL || o == mp_const_none)
        return SURF_EASE_LINEAR;
    const char *e = mp_obj_str_get_str(o);
    if (!strcmp(e, "linear")) return SURF_EASE_LINEAR;
    if (!strcmp(e, "in"))     return SURF_EASE_IN;
    if (!strcmp(e, "out"))    return SURF_EASE_OUT;
    if (!strcmp(e, "in_out")) return SURF_EASE_IN_OUT;
    mp_raise_ValueError(MP_ERROR_TEXT(
        "ease is 'linear', 'in', 'out' or 'in_out'"));
    return SURF_EASE_LINEAR;
}

static mp_obj_t node_tween(size_t n_args, const mp_obj_t *pos,
                           mp_map_t *kw)
{
    enum { ARG_prop, ARG_to, ARG_ms, ARG_ease, ARG_start };
    static const mp_arg_t spec[] = {
        {MP_QSTR_prop,  MP_ARG_REQUIRED | MP_ARG_OBJ, {.u_obj = MP_OBJ_NULL}},
        {MP_QSTR_to,    MP_ARG_REQUIRED | MP_ARG_OBJ, {.u_obj = MP_OBJ_NULL}},
        {MP_QSTR_ms,    MP_ARG_INT, {.u_int = 250}},
        {MP_QSTR_ease,  MP_ARG_OBJ, {.u_obj = MP_OBJ_NULL}},
        {MP_QSTR_start, MP_ARG_OBJ, {.u_obj = MP_OBJ_NULL}},
    };
    mp_arg_val_t a[MP_ARRAY_SIZE(spec)];
    mp_arg_parse_all(n_args - 1, pos + 1, kw, MP_ARRAY_SIZE(spec), spec, a);
    surf_node *n = node_of(pos[0]);
    int prop = tw_prop(a[ARG_prop].u_obj);
    /* `start` is applied first and WITHOUT cancelling, which is the
     * whole reason it exists: writing it through the public setter
     * would kill the tween we are about to make. */
    if (a[ARG_start].u_obj != MP_OBJ_NULL && a[ARG_start].u_obj != mp_const_none)
        surf_node_tween(n, (uint8_t)prop, tw_to_q16(prop, a[ARG_start].u_obj),
                        0, SURF_EASE_LINEAR);
    if (!surf_node_tween(n, (uint8_t)prop, tw_to_q16(prop, a[ARG_to].u_obj),
                         (int32_t)a[ARG_ms].u_int, tw_ease(a[ARG_ease].u_obj)))
        mp_raise_msg(&mp_type_TypeError,
                     MP_ERROR_TEXT("this node has no such property to tween "
                                   "- a group has a position but no opacity "
                                   "or scale"));
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_KW(node_tween_obj, 3, node_tween);

/* n.tween_cancel() stops everything on the node; n.tween_cancel("x_pos")
 * one property. Whatever it had reached is where it stays. */
static mp_obj_t node_tween_cancel(size_t n_args, const mp_obj_t *args)
{
    surf_node_tween_cancel(node_of(args[0]),
                           n_args > 1 ? tw_prop(args[1]) : -1);
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(node_tween_cancel_obj, 1, 2,
                                           node_tween_cancel);

static mp_obj_t node_tweening(size_t n_args, const mp_obj_t *args)
{
    return mp_obj_new_bool(surf_node_tweening(
        node_of(args[0]), n_args > 1 ? tw_prop(args[1]) : -1));
}
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(node_tweening_obj, 1, 2,
                                           node_tweening);

/* spr.fade_out(ms=250) / fade_in(ms=250) / fade_to(0.4, ms=250)
 *
 * ms is milliseconds of WALL time, driven by surf_tick — so a fade keeps
 * running through frames the app itself is not being called for, and
 * finishes rather than freezing half way. Writing .opacity directly
 * cancels a running fade; so does destroying the node.
 *
 * A node that cannot fade RAISES, the same as .opacity does, and for the
 * same reason: `group.fade_out()` doing nothing at all is a bug nobody
 * can see. */
static mp_obj_t node_fade_common(size_t n_args, const mp_obj_t *args,
                                 mp_float_t to, int arg_ms)
{
    mp_int_t ms = n_args > (size_t)arg_ms ? mp_obj_get_int(args[arg_ms]) : 250;
    if (to < 0) to = 0;
    if (to > 1) to = 1;
    if (!surf_node_fade_to(node_of(args[0]), (uint8_t)(to * 255 + (mp_float_t)0.5),
                           (int32_t)ms))
        mp_raise_msg(&mp_type_TypeError,
                     MP_ERROR_TEXT("only a sprite, filmstrip, ninepatch, "
                                   "layer or label can fade - a group "
                                   "cannot; fade the children"));
    return mp_const_none;
}
static mp_obj_t node_fade_out(size_t n_args, const mp_obj_t *args)
{
    return node_fade_common(n_args, args, 0, 1);
}
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(node_fade_out_obj, 1, 2, node_fade_out);

static mp_obj_t node_fade_in(size_t n_args, const mp_obj_t *args)
{
    return node_fade_common(n_args, args, 1, 1);
}
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(node_fade_in_obj, 1, 2, node_fade_in);

static mp_obj_t node_fade_to(size_t n_args, const mp_obj_t *args)
{
    return node_fade_common(n_args, args, mp_obj_get_float(args[1]), 2);
}
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(node_fade_to_obj, 2, 3, node_fade_to);

static mp_obj_t node_fade_cancel(mp_obj_t self_in)
{
    surf_node_fade_cancel(node_of(self_in));
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_1(node_fade_cancel_obj, node_fade_cancel);

static const mp_rom_map_elem_t node_locals_table[] = {
    {MP_ROM_QSTR(MP_QSTR_tween), MP_ROM_PTR(&node_tween_obj)},
    {MP_ROM_QSTR(MP_QSTR_tween_cancel), MP_ROM_PTR(&node_tween_cancel_obj)},
    {MP_ROM_QSTR(MP_QSTR_tweening), MP_ROM_PTR(&node_tweening_obj)},
    {MP_ROM_QSTR(MP_QSTR_fade_out), MP_ROM_PTR(&node_fade_out_obj)},
    {MP_ROM_QSTR(MP_QSTR_fade_in), MP_ROM_PTR(&node_fade_in_obj)},
    {MP_ROM_QSTR(MP_QSTR_fade_to), MP_ROM_PTR(&node_fade_to_obj)},
    {MP_ROM_QSTR(MP_QSTR_fade_cancel), MP_ROM_PTR(&node_fade_cancel_obj)},
    {MP_ROM_QSTR(MP_QSTR_add), MP_ROM_PTR(&node_add_obj)},
    {MP_ROM_QSTR(MP_QSTR_damage), MP_ROM_PTR(&node_damage_obj)},
    {MP_ROM_QSTR(MP_QSTR_hits), MP_ROM_PTR(&node_hits_obj)},
    {MP_ROM_QSTR(MP_QSTR_detach), MP_ROM_PTR(&node_detach_obj)},
    {MP_ROM_QSTR(MP_QSTR_destroy), MP_ROM_PTR(&node_destroy_obj)},
    {MP_ROM_QSTR(MP_QSTR_play), MP_ROM_PTR(&node_play_obj)},
    {MP_ROM_QSTR(MP_QSTR_stop), MP_ROM_PTR(&node_stop_obj)},
    {MP_ROM_QSTR(MP_QSTR_set_text), MP_ROM_PTR(&node_set_text_obj)},
    {MP_ROM_QSTR(MP_QSTR_set_color), MP_ROM_PTR(&node_set_color_obj)},
    {MP_ROM_QSTR(MP_QSTR_set_wrap), MP_ROM_PTR(&node_set_wrap_obj)},
    {MP_ROM_QSTR(MP_QSTR_set_align), MP_ROM_PTR(&node_set_align_obj)},
    {MP_ROM_QSTR(MP_QSTR_set_row), MP_ROM_PTR(&node_set_row_obj)},
    {MP_ROM_QSTR(MP_QSTR_set_cell), MP_ROM_PTR(&node_set_cell_obj)},
    {MP_ROM_QSTR(MP_QSTR_set_cells), MP_ROM_PTR(&node_set_cells_obj)},
    {MP_ROM_QSTR(MP_QSTR_grid_scroll), MP_ROM_PTR(&node_grid_scroll_obj)},
    {MP_ROM_QSTR(MP_QSTR_set_colors), MP_ROM_PTR(&node_set_colors_obj)},
    {MP_ROM_QSTR(MP_QSTR_set_clip), MP_ROM_PTR(&node_set_clip_obj)},
    {MP_ROM_QSTR(MP_QSTR_scrollback), MP_ROM_PTR(&node_scrollback_obj)},
    {MP_ROM_QSTR(MP_QSTR_view), MP_ROM_PTR(&node_view_obj)},
    {MP_ROM_QSTR(MP_QSTR_history), MP_ROM_PTR(&node_history_obj)},
    {MP_ROM_QSTR(MP_QSTR_set_offset), MP_ROM_PTR(&node_set_offset_obj)},
    {MP_ROM_QSTR(MP_QSTR_set_src), MP_ROM_PTR(&node_set_src_obj)},
    {MP_ROM_QSTR(MP_QSTR_set_image), MP_ROM_PTR(&node_set_image_obj)},
    {MP_ROM_QSTR(MP_QSTR_fast_scroll), MP_ROM_PTR(&node_fast_scroll_obj)},
    {MP_ROM_QSTR(MP_QSTR_scroll_to), MP_ROM_PTR(&node_scroll_to_obj)},
    {MP_ROM_QSTR(MP_QSTR_scroll_offset), MP_ROM_PTR(&node_scroll_offset_obj)},
    {MP_ROM_QSTR(MP_QSTR_insert), MP_ROM_PTR(&node_insert_obj)},
    {MP_ROM_QSTR(MP_QSTR_backspace), MP_ROM_PTR(&node_backspace_obj)},
    {MP_ROM_QSTR(MP_QSTR_delete), MP_ROM_PTR(&node_delete_obj)},
    {MP_ROM_QSTR(MP_QSTR_move), MP_ROM_PTR(&node_move_obj)},
    {MP_ROM_QSTR(MP_QSTR_focus), MP_ROM_PTR(&node_focus_obj)},
    {MP_ROM_QSTR(MP_QSTR_index_from_x), MP_ROM_PTR(&node_index_from_x_obj)},
    {MP_ROM_QSTR(MP_QSTR_key), MP_ROM_PTR(&node_key_obj)},
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
    /* THREE arguments, not four, even though the event now carries a
     * contact id. Adding it would break every `lambda phase, x, y:` in
     * every host, and MicroPython gives no portable way to ask a
     * callable how many arguments it takes — so the id would have to be
     * mandatory for everyone. The C widgets are where multitouch pays
     * (three fingers, three faders, all in dispatch), and Python code
     * that genuinely wants per-finger data has surfer.touches(), which
     * reports every contact with its id already. */
    mp_obj_t args[3] = {
        MP_OBJ_NEW_SMALL_INT(t->phase),
        MP_OBJ_NEW_SMALL_INT(t->x),
        MP_OBJ_NEW_SMALL_INT(t->y),
    };
    mp_call_function_n_kw(o->touch_cb, 3, 0, args);
}

/* A tap in a text field puts the caret where the finger landed, and a
 * drag extends the selection — that is what a text field IS, so the
 * binding wires it at creation rather than making every caller write it.
 * A Python on_touch still fires, after the caret has moved. */
static void ti_touch(surf_node *n, const surf_touch *t, void *user)
{
    surfer_node_obj_t *o = user;
    if (t->phase != SURF_TOUCH_UP) {
        int16_t ax, ay;
        surf_node_abs_pos(n, &ax, &ay);
        /* TWO DIMENSIONS WHERE THERE ARE TWO. `index_from_xy` is the
         * single-line call for a single-line field, so this is one
         * branch in the widget rather than one in every caller — and a
         * caller could not make it anyway, since `local_y` needs the
         * node's ABSOLUTE position and Python cannot walk up the tree
         * to find it. That gap is why tulip5's hand-rolled box had no
         * tap-to-place-the-caret at all. */
        surf_textinput_set_caret(
            n, surf_textinput_index_from_xy(n, (int16_t)(t->x - ax),
                                            (int16_t)(t->y - ay)),
            t->phase == SURF_TOUCH_MOVE);
    }
    if (o->touch_cb != mp_const_none)
        node_touch_tramp(n, t, user);
}

static void node_attr(mp_obj_t self_in, qstr attr, mp_obj_t *dest)
{
    surfer_node_obj_t *o = MP_OBJ_TO_PTR(self_in);
    if (!o->node) {
        if (dest[0] == MP_OBJ_NULL)
            dest[1] = MP_OBJ_SENTINEL;
        return;
    }
    if (o->is_input && dest[0] == MP_OBJ_NULL) {
        switch (attr) {
        case MP_QSTR_rows:                 /* 0 = a single-line field */
            dest[0] = MP_OBJ_NEW_SMALL_INT(surf_textinput_rows(o->node));
            return;
        case MP_QSTR_lines:                /* what the TEXT takes, wrapped */
            dest[0] = MP_OBJ_NEW_SMALL_INT(surf_textinput_lines(o->node));
            return;
        case MP_QSTR_scroll_y:
            dest[0] = MP_OBJ_NEW_SMALL_INT(surf_textinput_scroll_y(o->node));
            return;
        default:
            break;
        }
    }
    if (o->is_input && dest[0] != MP_OBJ_NULL && attr == MP_QSTR_scroll_y) {
        surf_textinput_scroll_to(o->node, (int16_t)mp_obj_get_int(dest[1]));
        dest[0] = MP_OBJ_NULL;
        return;
    }
    if (dest[0] == MP_OBJ_NULL && attr == MP_QSTR_on_touch) {
        dest[0] = o->touch_cb;
        return;
    }
    if (dest[0] != MP_OBJ_NULL && attr == MP_QSTR_on_touch) {
        o->touch_cb = dest[1];
        surf_node_set_on_touch(o->node,
                               o->is_input ? ti_touch : node_touch_tramp, o);
        dest[0] = MP_OBJ_NULL;
        return;
    }
    /* textinput: .text and .caret are the two things you read back */
    if (o->is_input && attr == MP_QSTR_text) {
        if (dest[0] == MP_OBJ_NULL) {
            const char *t = surf_textinput_text(o->node);
            dest[0] = mp_obj_new_str(t, strlen(t));
        } else {
            surf_textinput_set_text(o->node, mp_obj_str_get_str(dest[1]));
            dest[0] = MP_OBJ_NULL;
        }
        return;
    }
    if (o->is_input && attr == MP_QSTR_mask) {
        /* ti.mask = "*" makes it a password field; "" or None shows the
         * text again. The buffer is untouched either way — .text still
         * returns what was typed. */
        if (dest[0] == MP_OBJ_NULL) {
            char m = surf_textinput_mask(o->node);
            dest[0] = m ? mp_obj_new_str(&m, 1) : mp_const_none;
        } else {
            char m = 0;
            if (dest[1] != mp_const_none) {
                const char *t = mp_obj_str_get_str(dest[1]);
                m = t[0];
            }
            surf_textinput_set_mask(o->node, m);
            dest[0] = MP_OBJ_NULL;
        }
        return;
    }
    if (o->is_input && attr == MP_QSTR_caret) {
        if (dest[0] == MP_OBJ_NULL) {
            dest[0] = MP_OBJ_NEW_SMALL_INT(surf_textinput_caret(o->node));
        } else {
            surf_textinput_set_caret(o->node, mp_obj_get_int(dest[1]), false);
            dest[0] = MP_OBJ_NULL;
        }
        return;
    }
    /* filmstrip: .frame picks a cel, .fps plays it (0 = the caller
     * drives it, which is what an editor wants). Both guard on the node
     * type in C, so they are harmless no-ops on anything else. */
    if (dest[0] == MP_OBJ_NULL && attr == MP_QSTR_frame) {
        dest[0] = MP_OBJ_NEW_SMALL_INT(surf_filmstrip_frame(o->node));
        return;
    }
    if (dest[0] != MP_OBJ_NULL && attr == MP_QSTR_frame) {
        surf_filmstrip_set_frame(o->node, (int16_t)mp_obj_get_int(dest[1]));
        dest[0] = MP_OBJ_NULL;
        return;
    }
    if (dest[0] == MP_OBJ_NULL && attr == MP_QSTR_fps) {
        dest[0] = mp_obj_new_float((mp_float_t)surf_filmstrip_fps(o->node) /
                                   (mp_float_t)SURF_ONE);
        return;
    }
    if (dest[0] != MP_OBJ_NULL && attr == MP_QSTR_fps) {
        mp_float_t f = mp_obj_get_float(dest[1]);
        if (f < 0)
            f = 0;
        surf_filmstrip_set_fps(o->node, (int32_t)(f * (mp_float_t)SURF_ONE));
        dest[0] = MP_OBJ_NULL;
        return;
    }
    /* .playing: is the strip actually advancing (fps set AND not
     * stopped). Writable for symmetry; play()/stop() are the verbs. */
    if (dest[0] == MP_OBJ_NULL && attr == MP_QSTR_playing) {
        dest[0] = mp_obj_new_bool(surf_filmstrip_playing(o->node));
        return;
    }
    if (dest[0] != MP_OBJ_NULL && attr == MP_QSTR_playing) {
        surf_filmstrip_set_playing(o->node, mp_obj_is_true(dest[1]));
        dest[0] = MP_OBJ_NULL;
        return;
    }
    /* sprite transform: scale (float, 1.0 = 1:1), rot (degrees CCW,
     * quarter turns only — the P4 PPA's limit), mirror_x / mirror_y
     * (bools; source flip before rotation).
     *
     * WRITING one to a node that has no transform RAISES. Only sprites
     * and filmstrips carry one, and surf_sprite_set_xform quietly
     * returns for everything else — so `group.rot = 90` was accepted
     * and did nothing, which cost a Tulip user three model-assisted
     * rounds against a kitty that never turned. The set_color-on-a-
     * label trap, except set_color is a guarded no-op by documented
     * design and this was just a hole. Reads stay lenient (0 / False):
     * generic code asking a node what it is should get an answer. */
    if (dest[0] != MP_OBJ_NULL &&
        (attr == MP_QSTR_scale || attr == MP_QSTR_rot ||
         attr == MP_QSTR_mirror_x || attr == MP_QSTR_mirror_y) &&
        !surf_node_can_xform(o->node)) {
        mp_raise_msg(&mp_type_TypeError,
                     MP_ERROR_TEXT("only a sprite or filmstrip can "
                                   "scale/rot/mirror - a group cannot; "
                                   "transform each child"));
    }
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
        hb_dbg_sync(o);
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
        hb_dbg_sync(o);
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
        hb_dbg_sync(o);
        dest[0] = MP_OBJ_NULL;
        return;
    }
    /* .opacity: 0.0 gone .. 1.0 solid, over everything this node draws.
     *
     * A FLOAT and not 0..255, because what a caller has in its hand is a
     * fade's `t` — and an int would be ambiguous exactly where it hurts
     * (is `opacity = 1` solid, or 1/255 of it?).
     *
     * Writing to a node that cannot fade RAISES, the way .rot does: a
     * group, a rect and a textgrid have no opacity, and a silent no-op
     * on the wrong node type is this repo's most expensive recurring
     * bug. Reads stay lenient and answer 1.0, so generic code asking a
     * node what it is gets an answer. */
    if (dest[0] == MP_OBJ_NULL && attr == MP_QSTR_opacity) {
        dest[0] = mp_obj_new_float((mp_float_t)surf_node_opacity(o->node) /
                                   (mp_float_t)255);
        return;
    }
    if (dest[0] != MP_OBJ_NULL && attr == MP_QSTR_opacity) {
        mp_float_t f = mp_obj_get_float(dest[1]);
        if (f < 0) f = 0;
        if (f > 1) f = 1;
        if (!surf_node_set_opacity(o->node, (uint8_t)(f * 255 + (mp_float_t)0.5)))
            mp_raise_msg(&mp_type_TypeError,
                         MP_ERROR_TEXT("only a sprite, filmstrip, ninepatch, "
                                       "layer or label has opacity - a group "
                                       "cannot (it would need an offscreen "
                                       "target); fade the children"));
        dest[0] = MP_OBJ_NULL;
        return;
    }
    /* .fading: is a tween running right now. Read-only — fade_to() and
     * fade_cancel() are the verbs. */
    if (dest[0] == MP_OBJ_NULL && attr == MP_QSTR_fading) {
        dest[0] = mp_obj_new_bool(surf_node_fading(o->node));
        return;
    }
    if (dest[0] == MP_OBJ_NULL && attr == MP_QSTR_hitboxes) {
        if (!surf_node_can_xform(o->node))
            mp_raise_msg(&mp_type_TypeError,
                         MP_ERROR_TEXT("only a sprite or filmstrip has "
                                       "hitboxes"));
        dest[0] = hitboxes_obj_new(self_in);
        return;
    }
    bool storing = dest[0] != MP_OBJ_NULL;
    surf_point p = surf_node_pos(o->node);
    surf_point s = surf_node_size(o->node);
    node_pos_attr(o->node, attr, dest, p.x, p.y, s.x, s.y);
    /* debug outlines follow the sprite: a position store that was
     * consumed re-syncs them (event-driven -- never the frame path) */
    if (storing && dest[0] == MP_OBJ_NULL && o->hb_dbg != mp_const_none &&
        (attr == MP_QSTR_x_pos || attr == MP_QSTR_y_pos))
        hb_dbg_sync(o);
}

MP_DEFINE_CONST_OBJ_TYPE(surfer_node_type, MP_QSTR_Node, MP_TYPE_FLAG_NONE,
                         attr, node_attr, locals_dict, &node_locals_dict);

/* ---- hitboxes: n.hitboxes, its items, and the debug outlines --------
 *
 * Storage and collision are the core's (surf_hitbox_*); what lives here
 * is the LIST shape Python wants and the debug visualization. The
 * outlines are REAL RECT NODES in the sprite's parent -- an outline
 * drawn by the compose path outside the sprite's own box would break
 * dirty-rect damage accounting, and a box is allowed to hang outside
 * the picture, so the honest mechanism is scene nodes like any other
 * chrome. They are rebuilt on the mutations that move things (position,
 * transform, box edits), which is event-driven and never per frame; a
 * sprite riding a moving GROUP carries them for free, since they live
 * in the same parent. */

extern const mp_obj_type_t surfer_hitboxes_type;
extern const mp_obj_type_t surfer_hitbox_type;

typedef struct {
    mp_obj_base_t base;
    mp_obj_t node_ref;
} surfer_hitboxes_obj_t;

typedef struct {
    mp_obj_base_t base;
    mp_obj_t node_ref;
    int32_t idx;
} surfer_hitbox_obj_t;

#define HB_DBG_LINE 2
#define HB_DBG_DEFAULT SURF_RGB(255, 255, 255)

static void hb_entry_kill(mp_obj_t entry)
{
    if (entry == mp_const_none)
        return;
    size_t elen;
    mp_obj_t *e;
    mp_obj_list_get(entry, &elen, &e);
    if (e[0] != mp_const_none) {
        surfer_node_obj_t *g = MP_OBJ_TO_PTR(e[0]);
        if (g->node)
            surf_node_destroy(g->node);
        g->node = NULL;
        e[0] = mp_const_none;
    }
}

static void hb_dbg_teardown(void *node_obj)
{
    surfer_node_obj_t *o = node_obj;
    if (o->hb_dbg == mp_const_none)
        return;
    size_t len;
    mp_obj_t *entries;
    mp_obj_list_get(o->hb_dbg, &len, &entries);
    for (size_t i = 0; i < len; i++)
        hb_entry_kill(entries[i]);
    o->hb_dbg = mp_const_none;
}

static void hb_dbg_sync(void *node_obj)
{
    surfer_node_obj_t *o = node_obj;
    if (o->hb_dbg == mp_const_none || !o->node)
        return;
    size_t len;
    mp_obj_t *entries;
    mp_obj_list_get(o->hb_dbg, &len, &entries);
    surf_node *parent = surf_node_parent(o->node);
    for (size_t i = 0; i < len; i++) {
        if (entries[i] == mp_const_none)
            continue;
        size_t elen;
        mp_obj_t *e;
        mp_obj_list_get(entries[i], &elen, &e);
        /* rebuild from scratch: four rects is cheap, and an event */
        if (e[0] != mp_const_none) {
            surfer_node_obj_t *g = MP_OBJ_TO_PTR(e[0]);
            if (g->node)
                surf_node_destroy(g->node);
            g->node = NULL;
            e[0] = mp_const_none;
        }
        /* the entry EXISTING is not the entry being VISIBLE: a colour
         * set before (or after) visible=False must not draw anything.
         * Conflating the two is how "I set visible False and still see
         * it" happened — the colour line on the next row of the app
         * resurrected the outline. */
        if (!mp_obj_is_true(e[2]))
            continue;
        surf_rect r;
        if (!parent || !surf_hitbox_abs(o->node, (int32_t)i, &r) ||
            r.w <= 0 || r.h <= 0)
            continue;
        int16_t px, py;
        surf_node_abs_pos(parent, &px, &py);
        surf_color c = (surf_color)mp_obj_get_int(e[1]);
        surf_node *g = surf_group_new((int16_t)(r.x - px),
                                      (int16_t)(r.y - py));
        if (!g)
            continue;
        int16_t t = HB_DBG_LINE;
        if (r.w <= 2 * t || r.h <= 2 * t) {
            surf_node_add(g, surf_rect_new(0, 0, r.w, r.h, c));
        } else {
            surf_node_add(g, surf_rect_new(0, 0, r.w, t, c));
            surf_node_add(g, surf_rect_new(0, (int16_t)(r.h - t), r.w, t, c));
            surf_node_add(g, surf_rect_new(0, t, t, (int16_t)(r.h - 2 * t), c));
            surf_node_add(g, surf_rect_new((int16_t)(r.w - t), t, t,
                                           (int16_t)(r.h - 2 * t), c));
        }
        surf_node_add(parent, g);
        e[0] = MP_OBJ_FROM_PTR(new_node_obj(g));
    }
}

/* the entry list, grown on demand so indices always line up */
static mp_obj_t *hb_dbg_entry(surfer_node_obj_t *o, int32_t idx)
{
    if (o->hb_dbg == mp_const_none)
        o->hb_dbg = mp_obj_new_list(0, NULL);
    size_t len;
    mp_obj_t *entries;
    mp_obj_list_get(o->hb_dbg, &len, &entries);
    while ((int32_t)len <= idx) {
        mp_obj_list_append(o->hb_dbg, mp_const_none);
        mp_obj_list_get(o->hb_dbg, &len, &entries);
    }
    return &entries[idx];
}

static surfer_node_obj_t *hb_owner(mp_obj_t node_ref, int32_t idx)
{
    surfer_node_obj_t *o = MP_OBJ_TO_PTR(node_ref);
    if (!o->node)
        mp_raise_ValueError(MP_ERROR_TEXT("node destroyed"));
    if (idx >= 0 && idx >= surf_hitbox_count(o->node))
        mp_raise_msg(&mp_type_IndexError,
                     MP_ERROR_TEXT("hitbox removed or out of range"));
    return o;
}

static mp_obj_t hitbox_obj_new(mp_obj_t node_ref, int32_t idx)
{
    surfer_hitbox_obj_t *h = mp_obj_malloc(surfer_hitbox_obj_t,
                                           &surfer_hitbox_type);
    h->node_ref = node_ref;
    h->idx = idx;
    return MP_OBJ_FROM_PTR(h);
}

static mp_obj_t hitbox_remove(mp_obj_t self_in)
{
    surfer_hitbox_obj_t *h = MP_OBJ_TO_PTR(self_in);
    surfer_node_obj_t *o = hb_owner(h->node_ref, h->idx);
    /* later boxes shift down, so their debug entries shift with them */
    if (o->hb_dbg != mp_const_none) {
        size_t len;
        mp_obj_t *entries;
        mp_obj_list_get(o->hb_dbg, &len, &entries);
        if ((size_t)h->idx < len) {
            hb_entry_kill(entries[h->idx]);
            for (size_t k = h->idx; k + 1 < len; k++)
                entries[k] = entries[k + 1];
            entries[len - 1] = mp_const_none;
        }
    }
    surf_hitbox_remove(o->node, h->idx);
    hb_dbg_sync(o);
    h->idx = -1;                       /* this handle is dead */
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_1(hitbox_remove_obj, hitbox_remove);

static void hitbox_attr(mp_obj_t self_in, qstr attr, mp_obj_t *dest)
{
    surfer_hitbox_obj_t *h = MP_OBJ_TO_PTR(self_in);
    if (h->idx < 0)
        mp_raise_ValueError(MP_ERROR_TEXT("hitbox removed"));
    surfer_node_obj_t *o = hb_owner(h->node_ref, h->idx);
    surf_hitbox b;
    if (!surf_hitbox_get(o->node, h->idx, &b))
        mp_raise_msg(&mp_type_IndexError, MP_ERROR_TEXT("hitbox gone"));

    if (dest[0] == MP_OBJ_NULL) {                    /* load */
        switch (attr) {
        case MP_QSTR_x: dest[0] = MP_OBJ_NEW_SMALL_INT(b.x); return;
        case MP_QSTR_y: dest[0] = MP_OBJ_NEW_SMALL_INT(b.y); return;
        case MP_QSTR_w: dest[0] = MP_OBJ_NEW_SMALL_INT(b.w); return;
        case MP_QSTR_h: dest[0] = MP_OBJ_NEW_SMALL_INT(b.h); return;
        case MP_QSTR_visible: {
            bool on = false;
            if (o->hb_dbg != mp_const_none) {
                size_t len;
                mp_obj_t *entries;
                mp_obj_list_get(o->hb_dbg, &len, &entries);
                if ((size_t)h->idx < len &&
                    entries[h->idx] != mp_const_none) {
                    size_t elen;
                    mp_obj_t *e;
                    mp_obj_list_get(entries[h->idx], &elen, &e);
                    on = mp_obj_is_true(e[2]);
                }
            }
            dest[0] = mp_obj_new_bool(on);
            return;
        }
        case MP_QSTR_visible_color: {
            mp_obj_t *e = hb_dbg_entry(o, h->idx);
            if (*e == mp_const_none)
                dest[0] = MP_OBJ_NEW_SMALL_INT(HB_DBG_DEFAULT);
            else
                dest[0] = mp_obj_subscr(*e, MP_OBJ_NEW_SMALL_INT(1),
                                        MP_OBJ_SENTINEL);
            return;
        }
        case MP_QSTR_index:
            /* what hits() reports this box as -- the join between the
             * object add() returned and the tuple a collision answers.
             * NOTE remove() shifts LATER boxes down: an item held from
             * before an earlier box was removed is stale, so re-read
             * n.hitboxes[i] after removing rather than keeping old
             * handles around. */
            dest[0] = MP_OBJ_NEW_SMALL_INT(h->idx);
            return;
        case MP_QSTR_remove:
            dest[0] = MP_OBJ_FROM_PTR(&hitbox_remove_obj);
            dest[1] = self_in;
            return;
        default:
            return;
        }
    }
    /* store */
    switch (attr) {
    case MP_QSTR_x: surf_hitbox_set(o->node, h->idx,
        (int16_t)mp_obj_get_int(dest[1]), b.y, b.w, b.h); break;
    case MP_QSTR_y: surf_hitbox_set(o->node, h->idx,
        b.x, (int16_t)mp_obj_get_int(dest[1]), b.w, b.h); break;
    case MP_QSTR_w: surf_hitbox_set(o->node, h->idx,
        b.x, b.y, (int16_t)mp_obj_get_int(dest[1]), b.h); break;
    case MP_QSTR_h: surf_hitbox_set(o->node, h->idx,
        b.x, b.y, b.w, (int16_t)mp_obj_get_int(dest[1])); break;
    case MP_QSTR_visible: {
        mp_obj_t *e = hb_dbg_entry(o, h->idx);
        bool on = mp_obj_is_true(dest[1]);
        if (*e == mp_const_none) {
            mp_obj_t items[3] = {mp_const_none,
                                 MP_OBJ_NEW_SMALL_INT(HB_DBG_DEFAULT),
                                 mp_obj_new_bool(on)};
            *e = mp_obj_new_list(3, items);
        } else {
            mp_obj_subscr(*e, MP_OBJ_NEW_SMALL_INT(2), mp_obj_new_bool(on));
        }
        break;
    }
    case MP_QSTR_visible_color: {
        mp_obj_t *e = hb_dbg_entry(o, h->idx);
        if (*e == mp_const_none) {
            /* remember the colour, draw NOTHING: visibility is its own
             * switch and only visible=True throws it */
            mp_obj_t items[3] = {mp_const_none, dest[1], mp_const_false};
            *e = mp_obj_new_list(3, items);
        } else {
            mp_obj_subscr(*e, MP_OBJ_NEW_SMALL_INT(1), dest[1]);
        }
        break;
    }
    default:
        return;                                       /* not consumed */
    }
    hb_dbg_sync(o);
    dest[0] = MP_OBJ_NULL;
}

static const mp_rom_map_elem_t hitbox_locals_table[] = {
    {MP_ROM_QSTR(MP_QSTR_remove), MP_ROM_PTR(&hitbox_remove_obj)},
};
static MP_DEFINE_CONST_DICT(hitbox_locals_dict, hitbox_locals_table);

MP_DEFINE_CONST_OBJ_TYPE(surfer_hitbox_type, MP_QSTR_Hitbox,
                         MP_TYPE_FLAG_NONE, attr, hitbox_attr,
                         locals_dict, &hitbox_locals_dict);

static mp_obj_t hitboxes_add(size_t n_args, const mp_obj_t *args)
{
    (void)n_args;
    surfer_hitboxes_obj_t *hb = MP_OBJ_TO_PTR(args[0]);
    surfer_node_obj_t *o = hb_owner(hb->node_ref, -1);
    int32_t i = surf_hitbox_add(o->node,
                                (int16_t)mp_obj_get_int(args[1]),
                                (int16_t)mp_obj_get_int(args[2]),
                                (int16_t)mp_obj_get_int(args[3]),
                                (int16_t)mp_obj_get_int(args[4]));
    if (i < 0)
        mp_raise_ValueError(MP_ERROR_TEXT("hitbox add failed (32 max, "
                                          "w/h > 0)"));
    return hitbox_obj_new(hb->node_ref, i);
}
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(hitboxes_add_obj, 5, 5,
                                           hitboxes_add);

static mp_obj_t hitboxes_clear(mp_obj_t self_in)
{
    surfer_hitboxes_obj_t *hb = MP_OBJ_TO_PTR(self_in);
    surfer_node_obj_t *o = hb_owner(hb->node_ref, -1);
    hb_dbg_teardown(o);
    while (surf_hitbox_count(o->node))
        surf_hitbox_remove(o->node, 0);
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_1(hitboxes_clear_obj, hitboxes_clear);

static mp_obj_t hitboxes_unary(mp_unary_op_t op, mp_obj_t self_in)
{
    surfer_hitboxes_obj_t *hb = MP_OBJ_TO_PTR(self_in);
    surfer_node_obj_t *o = MP_OBJ_TO_PTR(hb->node_ref);
    mp_int_t n = o->node ? surf_hitbox_count(o->node) : 0;
    switch (op) {
    case MP_UNARY_OP_LEN: return MP_OBJ_NEW_SMALL_INT(n);
    case MP_UNARY_OP_BOOL: return mp_obj_new_bool(n != 0);
    default: return MP_OBJ_NULL;
    }
}

static mp_obj_t hitboxes_subscr(mp_obj_t self_in, mp_obj_t index,
                                mp_obj_t value)
{
    if (value != MP_OBJ_SENTINEL)
        mp_raise_msg(&mp_type_TypeError,
                     MP_ERROR_TEXT("edit a hitbox through its item"));
    surfer_hitboxes_obj_t *hb = MP_OBJ_TO_PTR(self_in);
    surfer_node_obj_t *o = MP_OBJ_TO_PTR(hb->node_ref);
    mp_int_t n = o->node ? surf_hitbox_count(o->node) : 0;
    mp_int_t i = mp_obj_get_int(index);
    if (i < 0)
        i += n;
    if (i < 0 || i >= n)
        mp_raise_msg(&mp_type_IndexError, MP_ERROR_TEXT("hitbox index"));
    return hitbox_obj_new(hb->node_ref, (int32_t)i);
}

static const mp_rom_map_elem_t hitboxes_locals_table[] = {
    {MP_ROM_QSTR(MP_QSTR_add), MP_ROM_PTR(&hitboxes_add_obj)},
    {MP_ROM_QSTR(MP_QSTR_clear), MP_ROM_PTR(&hitboxes_clear_obj)},
};
static MP_DEFINE_CONST_DICT(hitboxes_locals_dict, hitboxes_locals_table);

MP_DEFINE_CONST_OBJ_TYPE(surfer_hitboxes_type, MP_QSTR_Hitboxes,
                         MP_TYPE_FLAG_NONE, unary_op, hitboxes_unary,
                         subscr, hitboxes_subscr,
                         locals_dict, &hitboxes_locals_dict);

static mp_obj_t hitboxes_obj_new(mp_obj_t node_ref)
{
    surfer_hitboxes_obj_t *hb = mp_obj_malloc(surfer_hitboxes_obj_t,
                                              &surfer_hitboxes_type);
    hb->node_ref = node_ref;
    return MP_OBJ_FROM_PTR(hb);
}

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

/* img.flush() — publish pixels written through the buffer below.
 *
 * A no-op where the compositor reads the same memory the CPU wrote (SDL,
 * web), a cache writeback where the blitter is a DMA engine (the P4's PPA).
 * Call it after writing and before damaging the node, and the same code is
 * correct on a laptop and on the panel — which is the point, because getting
 * it wrong is invisible on the laptop. */
static mp_obj_t image_flush(mp_obj_t self_in)
{
    surfer_image_obj_t *o = MP_OBJ_TO_PTR(self_in);
    if (!o->img)
        mp_raise_ValueError(MP_ERROR_TEXT("image destroyed"));
    surf_image_flush(o->img);
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_1(image_flush_obj, image_flush);

/* The buffer protocol over an Image's own pixels: memoryview(img), or
 * mp_get_buffer() from another C module.
 *
 * This is what a software renderer needs and what Image had no answer for —
 * it could be drawn into with poly/line/fill and read back not at all, so
 * anything wanting to put its OWN pixels on the screen (a video decoder, a
 * camera, an emulator) had to reach through the binding's private struct.
 *
 * WRITABLE deliberately: read-only would make it a screenshot API, which
 * surfer already has in fb_read. Two things a caller must know, and both are
 * why `stride` is exposed beside it:
 *
 *   * ROWS ARE `stride` BYTES APART, NOT w*2. On the P4 every image
 *     allocation is 64-byte aligned in both directions, so a 100px RGB565
 *     row is 200 bytes of pixels inside a 256-byte stride. Code that assumes
 *     w*2 works on the desktop and shears on the panel.
 *   * the format is img.format — RGB565 is 2 bytes a pixel, ARGB8888 is 4,
 *     A8 is 1.
 *
 * len covers the whole allocation (h * stride), including any tail padding,
 * because that is what the pointer owns. */
static mp_int_t image_get_buffer(mp_obj_t self_in, mp_buffer_info_t *bufinfo,
                                 mp_uint_t flags)
{
    (void)flags;
    surfer_image_obj_t *o = MP_OBJ_TO_PTR(self_in);
    if (!o->img || !o->img->pixels)
        return 1;  /* destroyed: raises "object with buffer protocol required" */
    bufinfo->buf = o->img->pixels;
    bufinfo->len = (size_t)o->img->stride * o->img->h;
    bufinfo->typecode = 'B';
    return 0;
}

static const mp_rom_map_elem_t image_locals_table[] = {
    {MP_ROM_QSTR(MP_QSTR_flush), MP_ROM_PTR(&image_flush_obj)},
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
        /* BYTES per row, and not w * bytes-per-pixel: on the P4 an image is
         * 64-byte aligned in both directions, so a 100px RGB565 row is 200
         * bytes of pixels inside a 256-byte stride. Anyone walking the
         * buffer above needs this number rather than an assumption that
         * happens to hold on a desktop. */
        if (attr == MP_QSTR_stride) {
            dest[0] = MP_OBJ_NEW_SMALL_INT(o->img->stride);
            return;
        }
        /* surfer.RGB565 / .ARGB / .A8 — how wide a pixel in that buffer is */
        if (attr == MP_QSTR_format) {
            dest[0] = MP_OBJ_NEW_SMALL_INT(o->img->format);
            return;
        }
        /* WHERE the pixels are, as an integer. Not for arithmetic — the
         * buffer protocol is how you reach them — but for answering "did
         * this land in fast memory", which on a machine with a memory
         * hierarchy is the difference between 0.5 ms and 2.5 ms a frame
         * and is otherwise invisible from up here. On the P4, internal
         * SRAM reads 0x4ff..... and PSRAM does not. */
        if (attr == MP_QSTR_addr) {
            dest[0] = mp_obj_new_int_from_uint((uintptr_t)o->img->pixels);
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
                         attr, image_attr, buffer, image_get_buffer,
                         locals_dict, &image_locals_dict);

/* ---- Mesh: a low-poly .glb, software-rendered into an Image ---- */

static surf_mesh *mesh_of(mp_obj_t o)
{
    if (!mp_obj_is_type(o, &surfer_mesh_type))
        mp_raise_TypeError(MP_ERROR_TEXT("expected surfer Mesh"));
    surfer_mesh_obj_t *mo = MP_OBJ_TO_PTR(o);
    if (!mo->m)
        mp_raise_ValueError(MP_ERROR_TEXT("mesh destroyed"));
    return mo->m;
}

/* m.render(img, rx, ry, rz, size[, cx, cy[, cull]])
 *
 * Flat-shades the model into the image: rotations in degrees (ry spins,
 * rx tilts, rz rolls), size the model's radius in PIXELS, centered at
 * (cx, cy) — the image's own middle when omitted. cull None honors each
 * material's doubleSided flag, True always culls back faces (cheaper,
 * right for a closed model), False draws both sides.
 *
 * Like fill/poly/lines this does NOT flush: clear, render, then
 * img.flush() once and damage the sprite. Render small and let the
 * sprite's .scale enlarge it — that is the PPA on the device, and free. */
static mp_obj_t mesh_render(size_t n_args, const mp_obj_t *args)
{
    surf_mesh *m = mesh_of(args[0]);
    surf_image *dst = image_of(args[1]);
    float cx = n_args > 6 ? (float)mp_obj_get_float(args[6])
                          : (float)dst->w / 2.0f;
    float cy = n_args > 7 ? (float)mp_obj_get_float(args[7])
                          : (float)dst->h / 2.0f;
    int cull = -1;
    if (n_args > 8 && args[8] != mp_const_none)
        cull = mp_obj_is_true(args[8]) ? 1 : 0;
    if (!surf_mesh_render(m, dst,
                          (float)mp_obj_get_float(args[2]),
                          (float)mp_obj_get_float(args[3]),
                          (float)mp_obj_get_float(args[4]),
                          (float)mp_obj_get_float(args[5]),
                          cx, cy, cull))
        mp_raise_ValueError(MP_ERROR_TEXT("render needs an RGB565 or ARGB image"));
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(mesh_render_obj, 6, 9, mesh_render);

static mp_obj_t mesh_destroy(mp_obj_t self_in)
{
    surfer_mesh_obj_t *o = MP_OBJ_TO_PTR(self_in);
    if (o->m) {
        surf_mesh_destroy(o->m);
        o->m = NULL;
    }
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_1(mesh_destroy_obj, mesh_destroy);

static const mp_rom_map_elem_t mesh_locals_table[] = {
    {MP_ROM_QSTR(MP_QSTR_render), MP_ROM_PTR(&mesh_render_obj)},
    {MP_ROM_QSTR(MP_QSTR_destroy), MP_ROM_PTR(&mesh_destroy_obj)},
};
static MP_DEFINE_CONST_DICT(mesh_locals_dict, mesh_locals_table);

static void mesh_attr(mp_obj_t self_in, qstr attr, mp_obj_t *dest)
{
    surfer_mesh_obj_t *o = MP_OBJ_TO_PTR(self_in);
    if (dest[0] == MP_OBJ_NULL && attr == MP_QSTR_tris) {
        dest[0] = MP_OBJ_NEW_SMALL_INT(surf_mesh_tris(o->m));
        return;
    }
    if (dest[0] == MP_OBJ_NULL)
        dest[1] = MP_OBJ_SENTINEL;
}

MP_DEFINE_CONST_OBJ_TYPE(surfer_mesh_type, MP_QSTR_Mesh, MP_TYPE_FLAG_NONE,
                         attr, mesh_attr, locals_dict, &mesh_locals_dict);

/* ---- Widget ---- */

static void widget_cb(int32_t value, void *user)
{
    surfer_widget_obj_t *o = user;
    if (o->callback == mp_const_none || o->callback == MP_OBJ_NULL)
        return;
    /* EVERY kind is listed on purpose. Only the knob and the slider
     * report a Q16 fraction; a scrollbar reports the caller's own unit
     * (rows, lines, pixels) and a selector an index. When those two rode
     * a `default:` that divided by SURF_ONE they reported ~0 and every
     * `int(pos)` handler snapped to zero — twice, a year apart. So the
     * fraction is now the special case and the default is the raw int:
     * a new widget that forgets to add itself here gets a plausible
     * number instead of silently getting nothing. */
    mp_obj_t arg;
    switch (o->kind) {
    case W_SLIDER:
    case W_KNOB:      arg = mp_obj_new_float((mp_float_t)value / SURF_ONE); break;
    case W_CHECKBOX:  arg = mp_obj_new_bool(value != 0); break;
    case W_BUTTON:    arg = mp_const_true; break;
    case W_DROPDOWN:
    case W_SCROLLBAR:
    case W_SELECTOR:
    case W_TABS:
    case W_RADIO:
    case W_COLORPICKER:
    case W_LED:       /* has no callback, but be consistent */
    default:          arg = mp_obj_new_int(value); break;
    }
    mp_call_function_1(o->callback, arg);
}

/* knob.on_tap — a tap rather than a drag, with where in the knob's height
 * it landed. Separate from the value callback because it means something
 * else: not "the value moved" but "the user pointed at this". */
static void widget_tap_cb(int32_t frac, void *user)
{
    surfer_widget_obj_t *o = user;
    if (o->tap_cb == mp_const_none || o->tap_cb == MP_OBJ_NULL)
        return;
    mp_call_function_1(o->tap_cb, mp_obj_new_float((mp_float_t)frac / SURF_ONE));
}

static void widget_idx_cb(int32_t idx, void *user)
{
    widget_cb(idx, user);
}

static mp_obj_t widget_get_value(surfer_widget_obj_t *o)
{
    switch (o->kind) {
    case W_SCROLLBAR:
        return mp_obj_new_int(surf_scrollbar_pos(o->w));
    case W_SLIDER:
        return mp_obj_new_float(
            (mp_float_t)surf_slider_value(o->w) / SURF_ONE);
    case W_KNOB:
        return mp_obj_new_float((mp_float_t)surf_knob_value(o->w) / SURF_ONE);
    case W_CHECKBOX:
        return mp_obj_new_bool(surf_checkbox_checked(o->w));
    case W_LED:   /* a level, so a blink can fade; True/False also work */
        return mp_obj_new_float((mp_float_t)surf_led_level(o->w) / SURF_ONE);
    case W_SELECTOR:
        return MP_OBJ_NEW_SMALL_INT(surf_selector_index(o->w));
    case W_TABS:
        return MP_OBJ_NEW_SMALL_INT(surf_tabs_index(o->w));
    case W_RADIO:
        return MP_OBJ_NEW_SMALL_INT(surf_radio_index(o->w));
    case W_COLORPICKER:
        return MP_OBJ_NEW_SMALL_INT(surf_colorpicker_color(o->w));
    case W_BUTTON:
        return mp_const_none;
    default:
        return MP_OBJ_NEW_SMALL_INT(surf_dropdown_selected(o->w));
    }
}

static void widget_set_value(surfer_widget_obj_t *o, mp_obj_t v)
{
    switch (o->kind) {
    case W_SCROLLBAR:
        surf_scrollbar_set_pos(o->w, mp_obj_get_int(v));
        break;
    case W_SLIDER:
        surf_slider_set_value(o->w, (int32_t)(mp_obj_get_float(v) * SURF_ONE));
        break;
    case W_KNOB:
        surf_knob_set_value(o->w, (int32_t)(mp_obj_get_float(v) * SURF_ONE));
        break;
    case W_CHECKBOX:
        surf_checkbox_set_checked(o->w, mp_obj_is_true(v));
        break;
    case W_LED:
        /* led.value = True/False is the common case and reads better than
         * 1.0/0.0; anything else is a brightness */
        if (v == mp_const_true || v == mp_const_false)
            surf_led_set(o->w, v == mp_const_true);
        else
            surf_led_set_level(o->w, (int32_t)(mp_obj_get_float(v) * SURF_ONE));
        break;
    case W_SELECTOR:
        surf_selector_set_index(o->w, mp_obj_get_int(v));
        break;
    case W_TABS:
        surf_tabs_set_index(o->w, mp_obj_get_int(v));
        break;
    case W_RADIO:
        surf_radio_set_index(o->w, mp_obj_get_int(v));
        break;
    case W_COLORPICKER:
        surf_colorpicker_set_color(o->w, (surf_color)mp_obj_get_int(v));
        break;
    case W_BUTTON:
        break;
    default:
        surf_dropdown_set_selected(o->w, mp_obj_get_int(v));
        break;
    }
}

/* bar.set_range(total, visible, pos): what the content looks like now. */
static mp_obj_t widget_set_range(size_t n_args, const mp_obj_t *args)
{
    surfer_widget_obj_t *o = MP_OBJ_TO_PTR(args[0]);
    if (o->kind != W_SCROLLBAR)
        mp_raise_TypeError(MP_ERROR_TEXT("not a scrollbar"));
    surf_scrollbar_set_range(o->w, mp_obj_get_int(args[1]), mp_obj_get_int(args[2]),
                             n_args > 3 ? mp_obj_get_int(args[3]) : 0);
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(widget_set_range_obj, 3, 4,
                                           widget_set_range);

static void widget_attr(mp_obj_t self_in, qstr attr, mp_obj_t *dest)
{
    surfer_widget_obj_t *o = MP_OBJ_TO_PTR(self_in);
    if (dest[0] == MP_OBJ_NULL) {  /* load */
        if (attr == MP_QSTR_color) {
            surf_color c = 0;
            switch (o->kind) {
            case W_KNOB:   c = surf_knob_color(o->w); break;
            case W_SLIDER: c = surf_slider_color(o->w); break;
            default: break;
            }
            if (c) {
                dest[0] = MP_OBJ_NEW_SMALL_INT(c);
                return;
            }
        }
        if (attr == MP_QSTR_value) {
            dest[0] = widget_get_value(o);
            return;
        }
        if (attr == MP_QSTR_callback) {
            dest[0] = o->callback;
            return;
        }
        if (attr == MP_QSTR_node) {
            /* cached: a fresh wrapper per access leaked one object each
             * time, and `b.node.hidden` is written in a frame loop */
            if (o->node_ref == mp_const_none && o->node)
                o->node_ref = MP_OBJ_FROM_PTR(new_node_obj_raw(o->node));
            dest[0] = o->node_ref;
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
        if (attr == MP_QSTR_on_tap && o->kind == W_KNOB) {
            o->tap_cb = dest[1];
            surf_knob_on_tap(o->w, widget_tap_cb, o);
            dest[0] = MP_OBJ_NULL;
            return;
        }
        /* .color on anything drawn from A8 art: the lamp, and now the
         * knob, the selector and the slider's cap. One asset is any
         * colour because the tint is a property of the image STRUCT, not
         * of the pixels — a retint damages and repaints, and touches no
         * pixels at all. */
        if (attr == MP_QSTR_color) {
            surf_color c = (surf_color)mp_obj_get_int(dest[1]);
            switch (o->kind) {
            case W_LED:      surf_led_set_color(o->w, c); break;
            case W_KNOB:     surf_knob_set_color(o->w, c); break;
            case W_SELECTOR: surf_selector_set_color(o->w, c); break;
            /* the tabs' FACE colour: the page background it has to
               match. The dim one keeps whatever it was built with. */
            case W_TABS:     surf_tabs_set_face(o->w, c); break;
            case W_SLIDER:   surf_slider_set_color(o->w, c); break;
            default:         goto not_color;
            }
            dest[0] = MP_OBJ_NULL;
            return;
        }
    not_color:;
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

/* tabs.page(i) -> Node: the group behind tab i, for the caller to fill.
 * The widget shows and hides it; nothing else has to know it exists.
 *
 * Wrapped through the registry like every other node handed back from C,
 * so `t.page(0) is t.page(0)` and a Python attribute set on it survives.
 */
static mp_obj_t widget_page(mp_obj_t self_in, mp_obj_t idx)
{
    surfer_widget_obj_t *o = MP_OBJ_TO_PTR(self_in);
    if (o->kind != W_TABS)
        mp_raise_TypeError(MP_ERROR_TEXT("not a tabs widget"));
    surf_node *p = surf_tabs_page(o->w, mp_obj_get_int(idx));
    if (!p)
        mp_raise_ValueError(MP_ERROR_TEXT("no such tab"));
    return MP_OBJ_FROM_PTR(new_node_obj(p));
}
static MP_DEFINE_CONST_FUN_OBJ_2(widget_page_obj, widget_page);

/* tabs.set_label(i, text) — a tab whose legend follows what is in it. */
static mp_obj_t widget_set_label(mp_obj_t self_in, mp_obj_t idx, mp_obj_t text)
{
    surfer_widget_obj_t *o = MP_OBJ_TO_PTR(self_in);
    if (o->kind != W_TABS)
        mp_raise_TypeError(MP_ERROR_TEXT("not a tabs widget"));
    surf_tabs_set_label(o->w, mp_obj_get_int(idx), mp_obj_str_get_str(text));
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_3(widget_set_label_obj, widget_set_label);

/* tabs.set_face(i, color) / tabs.set_dim(i, color) — ONE tab's two
 * tints, for a strip whose pages each carry their own paper: the
 * bright face is that page's background (the join rule, per tab) and
 * the dim one a darker shade of it, so an unselected tab still says
 * which page it opens. `.color` keeps its old meaning — every face at
 * once. */
static mp_obj_t widget_tab_face(mp_obj_t self_in, mp_obj_t idx,
                                mp_obj_t col)
{
    surfer_widget_obj_t *o = MP_OBJ_TO_PTR(self_in);
    if (o->kind != W_TABS)
        mp_raise_TypeError(MP_ERROR_TEXT("not a tabs widget"));
    surf_tabs_set_face_at(o->w, mp_obj_get_int(idx),
                          (surf_color)mp_obj_get_int(col));
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_3(widget_tab_face_obj, widget_tab_face);

static mp_obj_t widget_tab_dim(mp_obj_t self_in, mp_obj_t idx,
                               mp_obj_t col)
{
    surfer_widget_obj_t *o = MP_OBJ_TO_PTR(self_in);
    if (o->kind != W_TABS)
        mp_raise_TypeError(MP_ERROR_TEXT("not a tabs widget"));
    surf_tabs_set_dim_at(o->w, mp_obj_get_int(idx),
                         (surf_color)mp_obj_get_int(col));
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_3(widget_tab_dim_obj, widget_tab_dim);

static const mp_rom_map_elem_t widget_locals_table[] = {
    {MP_ROM_QSTR(MP_QSTR_detach), MP_ROM_PTR(&widget_detach_obj)},
    {MP_ROM_QSTR(MP_QSTR_set_range), MP_ROM_PTR(&widget_set_range_obj)},
    {MP_ROM_QSTR(MP_QSTR_page), MP_ROM_PTR(&widget_page_obj)},
    {MP_ROM_QSTR(MP_QSTR_set_label), MP_ROM_PTR(&widget_set_label_obj)},
    {MP_ROM_QSTR(MP_QSTR_set_face), MP_ROM_PTR(&widget_tab_face_obj)},
    {MP_ROM_QSTR(MP_QSTR_set_dim), MP_ROM_PTR(&widget_tab_dim_obj)},
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
    o->tap_cb = mp_const_none;
    o->node_ref = mp_const_none;
    /* the widget owns its root node's slot; .node hangs off the widget */
    registry_set(node, MP_OBJ_FROM_PTR(o));
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

/* surfer.fb_image(x=0, y=0, w=all, h=all) -> Image
 *
 * The framebuffer as an ordinary Image, so everything that already takes
 * one takes a screenshot: write_png() to encode it, image_scale() to
 * make a thumbnail, blit() to put it in something, the buffer protocol
 * to read the bytes.
 *
 * THIS IS THE PIECE THAT WAS MISSING, and its absence was expensive
 * rather than merely awkward. A caller wanting a PNG of the screen had
 * exactly two doors and neither led anywhere: screenshot() writes a PPM
 * and only where the port implements it (desktop and web), and
 * write_png() takes an Image and never the screen. So the only route
 * was fb_read() -> RGB888 bytes -> a PER-PIXEL PYTHON LOOP repacking
 * them to 565 -> image_new. That is 42 ms of MicroPython for one
 * 1024x546 screen on a laptop and seconds of it on an ESP32-P4, to do
 * a job that is a memcpy per row: the framebuffer is RGB565 and so is
 * an Image, so nothing needs converting at all.
 *
 * It is a COPY, not a view. A view would alias memory the compositor
 * writes whenever anything is damaged, so the picture would change
 * under whoever was encoding it — and freeing it would free the screen.
 *
 * fb_read() stays: it answers in RGB888, which is what a test comparing
 * pixels or a caller writing a PPM actually wants, and it needs no
 * image allocation to answer a four-pixel question. */
static mp_obj_t mod_fb_image(size_t n_args, const mp_obj_t *args)
{
    if (!g_hal || !g_hal->fb_ptr)
        mp_raise_msg(&mp_type_RuntimeError, MP_ERROR_TEXT("no framebuffer"));
    mp_int_t x = n_args > 0 ? mp_obj_get_int(args[0]) : 0;
    mp_int_t y = n_args > 1 ? mp_obj_get_int(args[1]) : 0;
    mp_int_t w = n_args > 2 ? mp_obj_get_int(args[2]) : g_scr_w - x;
    mp_int_t h = n_args > 3 ? mp_obj_get_int(args[3]) : g_scr_h - y;
    if (x < 0 || y < 0 || w <= 0 || h <= 0 ||
        x + w > g_scr_w || y + h > g_scr_h)
        mp_raise_ValueError(MP_ERROR_TEXT("region out of bounds"));
    surfer_port_fb_sync_for_read();
    int32_t stride;
    const uint8_t *fb = g_hal->fb_ptr(&stride);
    surf_image *img = surf_image_new((int16_t)w, (int16_t)h, SURF_FMT_RGB565);
    if (!img)
        mp_raise_ValueError(MP_ERROR_TEXT("image_new failed"));
    for (mp_int_t j = 0; j < h; j++)
        memcpy((uint8_t *)img->pixels + (size_t)j * img->stride,
               fb + (size_t)(y + j) * stride + (size_t)x * 2,
               (size_t)w * 2);
    /* The CPU wrote those bytes and on the P4 the blitter reads memory,
     * so publish them the way every other producer here does. */
    surf_image_flush(img);
    surfer_image_obj_t *o = mp_obj_malloc(surfer_image_obj_t,
                                          &surfer_image_type);
    o->img = img;
    return MP_OBJ_FROM_PTR(o);
}
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(mod_fb_image_obj, 0, 4,
                                           mod_fb_image);

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
    /* 4096: real apps blow 512 fast — tulip5's drum machine alone holds
     * ~1100 live nodes (8 channel strips + a 155-row sound chooser) —
     * and 2048 then ran out with SIX ordinary apps open at once, which
     * an OS with a task bar is expected to do. Exhaustion raises
     * RuntimeError mid-scene-build, which presents as a half-alive UI
     * (everything built before the throw works, nothing after does) —
     * found the hard way.
     *
     * The cost is RAM and ONLY RAM: pool_cap is read in surf_init and
     * nowhere else (node.c), and compose walks the tree with a paint
     * list bounded by what intersects the dirty rect, not by capacity.
     * So raising it costs nothing per frame. 136 B/surf_node + 24 B/
     * surf_paint_ent = 160 B a slot: 320 KB at 2048, 640 KB at 4096.
     * On the P4 that calloc lands in PSRAM (SPIRAM_USE_MALLOC, and
     * SPIRAM_MALLOC_ALWAYSINTERNAL is 16 KB), so the tight backend is
     * the browser, not the board. */
    surf_config cfg = {.max_nodes = SURF_POOL_NODES,
                       .bg = SURF_RGB(18, 20, 25)};
    if (inited) {
        /* soft reset (or repeat init): the VM dropped every Python object,
         * so rebuild the C scene from scratch on the surviving hal —
         * stale nodes with dangling callbacks must not outlive the VM.
         * The slot table died with the old heap; drop the root pointer so
         * it is rebuilt rather than written into freed memory (a store
         * fault on the first node after Ctrl-D otherwise). This is the
         * only per-session reset surfer gets — a built-in module's
         * __init__ is not it, since a const-globals module never lands in
         * sys.modules and so runs __init__ on EVERY import, not once. */
        MP_STATE_VM(surfer_nodes) = MP_OBJ_NULL;
        /* and the chrome goes back to the house default, so a fresh VM
         * does not inherit a face the last one chose */
        widget_font_cache = NULL;
        widget_font_name[0] = '\0';
        widget_font_unnamed = false;
        surf_deinit();
        /* ...AND A DIFFERENT SIZE NEEDS A DIFFERENT DISPLAY. The hal
         * allocates its framebuffer at the size it was built with and
         * has no way to be told otherwise, so re-initialising the CORE
         * at a new size while keeping the old hal composes w-wide rows
         * into the wrong stride — it runs, and writes past the end of
         * every row. Hosts whose screen can change shape under them (a
         * phone turned on its side, a resizable window) are exactly the
         * ones that reach this, so the display is re-shaped in place —
         * IN PLACE, because tearing it down destroys the platform's own
         * window and, on iOS, left a keyboard notification aimed at a
         * view controller that had gone (a segfault inside UIKit on the
         * second rotation).
         *
         * The same size keeps the hal it has, which is the common case
         * by far — a soft reset — and is what makes this free where
         * nothing moved. A panel port never sees it at all: the size it
         * reports cannot change.
         *
         * prepare_assets() is deliberately NOT re-run. It re-homes each
         * atlas ONCE, in place (on the device, flash .rodata into a
         * fresh PSRAM allocation), so a second pass would leak the
         * first copy and re-do the work; those pixels belong to the
         * process, not to the hal, and survive it. */
        if ((w != g_scr_w || h != g_scr_h) && !surfer_port_resize(w, h))
            mp_raise_msg(&mp_type_RuntimeError,
                         MP_ERROR_TEXT("display cannot change size"));
        g_scr_w = w;
        g_scr_h = h;
        if (!surf_init(g_hal, w, h, &cfg))
            mp_raise_msg(&mp_type_RuntimeError, MP_ERROR_TEXT("surf re-init failed"));
        registry_init();
        return mp_const_none;
    }
    g_hal = surfer_port_init(w, h, single);
    g_scr_w = w;
    g_scr_h = h;
    if (!g_hal)
        mp_raise_msg(&mp_type_RuntimeError, MP_ERROR_TEXT("display init failed"));
    /* surf_init FIRST, then the assets. prepare_assets() re-homes every
     * atlas, and a 1-bit font atlas is UNPACKED on the way (see
     * SURF_FMT_A1) — which needs an allocator, and the core's is
     * surf_g.hal, set by surf_init. Re-homing first left every A1 atlas
     * packed, and a packed atlas read as A8 draws its bits as alpha:
     * text came out as coloured noise. Nothing in surf_init reads an
     * atlas, so the order is free. */
    if (!surf_init(g_hal, w, h, &cfg))
        mp_raise_msg(&mp_type_RuntimeError, MP_ERROR_TEXT("surf init failed"));
    prepare_assets();
    registry_init();
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
        /* (kind, text, shift, ctrl). FOUR, and the fourth is why every
         * `for kind, text, shift in surfer.keys()` in the tree had to be
         * widened when ctrl arrived. The cheap alternative was to make
         * the third element a modifier BITMASK — nothing would have had
         * to change, since bit 0 is shift and `if shift:` stays true —
         * and it is wrong: `if shift:` would then also be true with
         * ctrl alone held, so ctrl+Left would extend a selection. A
         * modifier nobody asked about must read as false. */
        mp_obj_t t[4] = {
            MP_OBJ_NEW_SMALL_INT(k.kind),
            mp_obj_new_str(k.utf8, strlen(k.utf8)),
            mp_obj_new_bool(k.shift),
            mp_obj_new_bool(k.ctrl),
        };
        mp_obj_list_append(list, mp_obj_new_tuple(4, t));
    }
    return list;
}
static MP_DEFINE_CONST_FUN_OBJ_0(mod_keys_obj, mod_keys);

/* surfer.wheel() -> [(x, y, dx, dy), ...]: wheel / two-finger pushes
 * that NO SCROLLVIEW TOOK, drained like keys().
 *
 * The scrollviews under the pointer get first refusal, so a dialog's
 * file list still scrolls while the same gesture over the app behind it
 * arrives here — the bargain touch already makes. dx/dy are pixels of
 * CONTENT movement, the direction a drag would have gone, and x/y are
 * framebuffer pixels, mapped exactly as a touch is.
 *
 * It is what a wheel means when it does not mean scrolling: zoom a
 * picture, step a value, spin a knob. On a laptop it is also the only
 * two-finger gesture there IS — the SDL hal feeds touches() from DIRECT
 * touch devices only, so a trackpad is a mouse and a wheel and a pinch
 * there can never arrive. */
static mp_obj_t mod_wheel(void)
{
    mp_obj_t list = mp_obj_new_list(0, NULL);
    surf_wheel w;
    while (surf_wheel_poll(&w)) {
        mp_obj_t t[4] = {
            MP_OBJ_NEW_SMALL_INT(w.x), MP_OBJ_NEW_SMALL_INT(w.y),
            MP_OBJ_NEW_SMALL_INT(w.dx), MP_OBJ_NEW_SMALL_INT(w.dy),
        };
        mp_obj_list_append(list, mp_obj_new_tuple(4, t));
    }
    return list;
}
static MP_DEFINE_CONST_FUN_OBJ_0(mod_wheel_obj, mod_wheel);

/* surfer.screen_keyboard() -> True/False/None, surfer.screen_keyboard(on).
 *
 * The platform's own on-glass keyboard (iOS). None means the platform
 * has not got one — a desktop, the P4 — and is the gate a caller draws
 * its toggle behind: a keyboard button on a machine whose keyboard is
 * physical is chrome that can only do nothing. With an argument it
 * summons or dismisses; either way the answer is what is NOW shown,
 * which can disagree with what was asked — the OS animates it, and the
 * user can dismiss it from its own key without anyone being told. */
static mp_obj_t mod_screen_keyboard(size_t n_args, const mp_obj_t *args)
{
    int op = -1;
    if (n_args == 1)
        op = mp_obj_is_true(args[0]) ? 1 : 0;
    int r = surf_screen_keyboard(op);
    if (r < 0)
        return mp_const_none;
    return mp_obj_new_bool(r);
}
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(mod_screen_keyboard_obj, 0, 1,
                                           mod_screen_keyboard);

/* surfer._wheel(x, y, dx, dy) — push one through the normal path, so a
 * headless test can reach anything that reads a wheel. The counterpart
 * of _touch and _key, and it goes through surf_input_wheel rather than
 * straight into the queue: a test should find out that a scrollview
 * under the pointer eats it. */
static mp_obj_t mod_wheel_inject(size_t n_args, const mp_obj_t *args)
{
    (void)n_args;
    surf_input_wheel((int16_t)mp_obj_get_int(args[0]),
                     (int16_t)mp_obj_get_int(args[1]),
                     (int16_t)mp_obj_get_int(args[2]),
                     (int16_t)mp_obj_get_int(args[3]));
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(mod_wheel_inject_obj, 4, 4,
                                           mod_wheel_inject);

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
/* surfer.image_new(w, h, fmt=0, fast=False)
 *
 * `fast` asks for memory the CPU writes quickly, for an image something
 * renders into EVERY FRAME rather than bakes once -- an emulator's
 * screen, a decoder's surface. It is a hint: where there is no such
 * memory, or none left, you get an ordinary image and no error. */
static mp_obj_t mod_image_new(size_t n_args, const mp_obj_t *args)
{
    /* 0/False = opaque 565, 1/True = ARGB, surfer.A8 = tintable mask */
    mp_int_t fmt = n_args > 2 ? mp_obj_get_int(args[2]) : 0;
    bool fast = n_args > 3 && mp_obj_is_true(args[3]);
    int16_t w = (int16_t)mp_obj_get_int(args[0]);
    int16_t h = (int16_t)mp_obj_get_int(args[1]);
    surf_image *img = fast ? surf_image_new_fast(w, h, (surf_format)fmt)
                           : surf_image_new(w, h, (surf_format)fmt);
    if (!img)
        mp_raise_ValueError(MP_ERROR_TEXT("image_new failed"));
    surfer_image_obj_t *o = mp_obj_malloc(surfer_image_obj_t, &surfer_image_type);
    o->img = img;
    return MP_OBJ_FROM_PTR(o);
}
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(mod_image_new_obj, 2, 4, mod_image_new);

/* surfer.mesh(glb_bytes[, tex_png_bytes[, textured]]) -> Mesh
 *
 * Loads a glTF 2 binary (.glb): triangles, node transforms flattened,
 * one flat color per face — the material's baseColorFactor times its
 * texture sampled at the face's UV centroid (exact for palette-textured
 * low-poly art) or the COLOR_0 centroid. A texture the file references
 * by URI (Kenney keeps one colormap.png beside its models) comes from
 * the second argument; an embedded texture needs nothing. The model is
 * centered and normalized, so render's size is its radius in pixels.
 * Loading is the expensive half — do it once, at build time.
 *
 * textured=True keeps the textures resident and samples them per PIXEL
 * at render instead of once per face at load — for painted models
 * whose detail lives inside a face, where the centroid sample collapses
 * every face to one color. Costs the texture's memory for the mesh's
 * lifetime and a slower render; harmless on a model with no texture. */
static mp_obj_t mod_mesh(size_t n_args, const mp_obj_t *args)
{
    mp_buffer_info_t glb, tex = {0};
    mp_get_buffer_raise(args[0], &glb, MP_BUFFER_READ);
    if (n_args > 1 && args[1] != mp_const_none)
        mp_get_buffer_raise(args[1], &tex, MP_BUFFER_READ);
    unsigned flags = (n_args > 2 && mp_obj_is_true(args[2]))
                         ? SURF_MESH_TEXTURED : 0;
    char err[64];
    surf_mesh *m = surf_mesh_from_glb(glb.buf, glb.len, tex.buf, tex.len,
                                      flags, err, sizeof err);
    if (!m)
        mp_raise_msg_varg(&mp_type_ValueError, MP_ERROR_TEXT("glb: %s"), err);
    surfer_mesh_obj_t *o = mp_obj_malloc(surfer_mesh_obj_t, &surfer_mesh_type);
    o->m = m;
    return MP_OBJ_FROM_PTR(o);
}
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(mod_mesh_obj, 1, 3, mod_mesh);

/* surfer.image_scale(dst, src) -> True if the HARDWARE did it
 *
 * Scales the whole of src into the whole of dst. Down then up is a blur,
 * which on a backend with a 2D engine costs no CPU at all -- see
 * surf_image_scale in surfer.h. The return value is not success (it
 * always works, falling back to a CPU bilinear); it says whether it was
 * free. */
static mp_obj_t mod_image_scale(mp_obj_t dst_in, mp_obj_t src_in)
{
    surfer_image_obj_t *d = MP_OBJ_TO_PTR(dst_in);
    surfer_image_obj_t *s = MP_OBJ_TO_PTR(src_in);
    if (!mp_obj_is_type(dst_in, &surfer_image_type) ||
        !mp_obj_is_type(src_in, &surfer_image_type))
        mp_raise_TypeError(MP_ERROR_TEXT("image_scale wants two Images"));
    return mp_obj_new_bool(surf_image_scale(d->img, s->img));
}
static MP_DEFINE_CONST_FUN_OBJ_2(mod_image_scale_obj, mod_image_scale);

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

/* surfer.filmstrip(image, frame_w, frame_h, x, y) -> Node
 *
 * The node type has been in the core since M1 — it is how checkbox,
 * knob, led and selector are drawn — and was never bound, so from
 * Python a strip of frames was a sprite you had to set_src by hand
 * every time. An ANIMATION is exactly this node: one image, uniform
 * frames left to right, `.frame` picking one.
 *
 * `.fps` is the binding's own and advances `.frame` from tick, wrapping
 * — a caller that wants to drive it by hand leaves fps at 0, which is
 * what a frame PICKER (an editor) wants and what a game that steps the
 * walk cycle off its own physics wants too.
 *
 * `.play([fps])` / `.stop()` / `.playing` are the TRANSPORT: stop
 * freezes the strip keeping its fps, play resumes at that speed (or
 * sets a new one first). fps is the SPEED and 0 means caller-driven;
 * the transport is whether it runs — two different questions. */
static mp_obj_t mod_filmstrip(size_t n_args, const mp_obj_t *args)
{
    (void)n_args;
    if (!mp_obj_is_type(args[0], &surfer_image_type))
        mp_raise_TypeError(MP_ERROR_TEXT("expected surfer Image"));
    surfer_image_obj_t *io = MP_OBJ_TO_PTR(args[0]);
    if (!io->img)
        mp_raise_ValueError(MP_ERROR_TEXT("image destroyed"));
    int16_t fw = (int16_t)mp_obj_get_int(args[1]);
    int16_t fh = (int16_t)mp_obj_get_int(args[2]);
    if (fw <= 0 || fh <= 0)
        mp_raise_ValueError(MP_ERROR_TEXT("frame size must be positive"));
    surf_node *n = surf_filmstrip_new(io->img, fw, fh,
                                      (int16_t)mp_obj_get_int(args[3]),
                                      (int16_t)mp_obj_get_int(args[4]));
    if (!n)
        mp_raise_ValueError(MP_ERROR_TEXT("filmstrip create failed"));
    surfer_node_obj_t *o = new_node_obj(n);
    o->img_ref = args[0];
    return MP_OBJ_FROM_PTR(o);
}
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(mod_filmstrip_obj, 5, 5,
                                           mod_filmstrip);

/* surfer.write_png(image) -> bytes
 *
 * The other half of surfer.image(). An image can be drawn into and, until
 * this, never saved — see surf_image_to_png for why it is C and not a
 * few lines of Python. */
static mp_obj_t mod_write_png(mp_obj_t img_in)
{
    if (!mp_obj_is_type(img_in, &surfer_image_type))
        mp_raise_TypeError(MP_ERROR_TEXT("expected surfer Image"));
    surfer_image_obj_t *io = MP_OBJ_TO_PTR(img_in);
    if (!io->img)
        mp_raise_ValueError(MP_ERROR_TEXT("image destroyed"));
    size_t len = 0;
    void *png = surf_image_to_png(io->img, &len);
    if (!png)
        mp_raise_ValueError(MP_ERROR_TEXT("png encode failed"));
    mp_obj_t out = mp_obj_new_bytes((const byte *)png, len);
    surf_image_png_free(png);
    return out;
}
static MP_DEFINE_CONST_FUN_OBJ_1(mod_write_png_obj, mod_write_png);

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
    mp_obj_t fref = mp_const_none;
    const surf_font *f = n_args > 4 ? font_arg(args[4], &fref)
                                    : font_named(DEFAULT_FONT);
    surfer_node_obj_t *o = new_node_obj(surf_text_new(
        f, mp_obj_str_get_str(args[0]), mp_obj_get_int(args[1]),
        mp_obj_get_int(args[2]), c));
    if (o)
        o->img_ref = fref;              /* keeps a runtime Font alive */
    return MP_OBJ_FROM_PTR(o);
}
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(mod_label_obj, 3, 5, mod_label);

/* surfer.text_image(str, color, font, wrap_w) -> Image (ARGB)
 *
 * A LABEL BAKED INTO PIXELS, for the one thing a label cannot do:
 * scale. Show it with a sprite and set .scale — bake at the face's own
 * size, let the compositor enlarge, which is the "bake at final size"
 * rule pointed the other way. The Image is the caller's to destroy;
 * unlike a label it holds no reference to a runtime Font, because the
 * pixels are copied out at the call. */
static mp_obj_t mod_text_image(size_t n_args, const mp_obj_t *args)
{
    surf_color c = n_args > 1 ? (surf_color)mp_obj_get_int(args[1])
                              : SURF_RGB(240, 242, 248);
    mp_obj_t fref = mp_const_none;
    const surf_font *f = n_args > 2 ? font_arg(args[2], &fref)
                                    : font_named(DEFAULT_FONT);
    int16_t wrap = n_args > 3 ? (int16_t)mp_obj_get_int(args[3]) : 0;
    surf_image *img = surf_text_bake(f, mp_obj_str_get_str(args[0]), c, wrap);
    if (!img)
        mp_raise_ValueError(MP_ERROR_TEXT("text_image failed"));
    surfer_image_obj_t *o = mp_obj_malloc(surfer_image_obj_t,
                                          &surfer_image_type);
    o->img = img;
    return MP_OBJ_FROM_PTR(o);
}
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(mod_text_image_obj, 1, 4,
                                           mod_text_image);

/* surfer.textinput(x, y, w, color, font) -> Node
 *
 * One line of editable text with a caret and a selection. It draws the
 * TEXT only — no box, no border, no keyboard: those are the caller's
 * (DESIGN.md §2.5), which is why a field is usually a rect with one of
 * these on top. Taps place the caret; feed it keys with .key(). */
static mp_obj_t mod_textinput(size_t n_args, const mp_obj_t *args)
{
    surf_color c = n_args > 3 ? (surf_color)mp_obj_get_int(args[3])
                              : SURF_RGB(240, 242, 248);
    mp_obj_t fref = mp_const_none;
    const surf_font *f = n_args > 4 ? font_arg(args[4], &fref)
                                    : font_named(DEFAULT_FONT);
    surfer_node_obj_t *o = new_node_obj(surf_textinput_new(
        f, mp_obj_get_int(args[0]), mp_obj_get_int(args[1]),
        mp_obj_get_int(args[2]), c));
    o->img_ref = fref;                  /* keeps a runtime Font alive */
    o->is_input = true;
    surf_node_set_on_touch(o->node, ti_touch, o);
    return MP_OBJ_FROM_PTR(o);
}
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(mod_textinput_obj, 3, 5, mod_textinput);

/* surfer.textarea(x, y, w, rows, color=, font=) -> Node
 *
 * The MULTILINE field, and the thing every host was hand-rolling
 * without one. textinput is a LINE, so a paragraph meant a textgrid
 * with a caret painted into it by hand — which is monospaced by
 * construction (a textgrid refuses a proportional face), re-wrapped in
 * the host language on every keystroke, and a second opinion about
 * where a word breaks.
 *
 * This is the same node as textinput with a wrap and a height, so the
 * buffer, the caret, the selection and every edit are the ones already
 * there — and the wrap is `surf_tlayout`'s, the walker the label and
 * the paint already use. Proportional faces, kerning and the emoji
 * fallback all come free for exactly that reason.
 *
 * `rows` is LINES, not pixels: a face is the host's preference, so a
 * field asked for as five lines is five lines of whatever it is given.
 * Enter is the caller's — `node.key()` returns False for it, the way it
 * always has — because only the caller knows whether Enter means a new
 * line or `submit`. */
static mp_obj_t mod_textarea(size_t n_args, const mp_obj_t *args)
{
    surf_color c = n_args > 4 ? (surf_color)mp_obj_get_int(args[4])
                              : SURF_RGB(240, 242, 248);
    mp_obj_t fref = mp_const_none;
    const surf_font *f = n_args > 5 ? font_arg(args[5], &fref)
                                    : font_named(DEFAULT_FONT);
    surfer_node_obj_t *o = new_node_obj(surf_textarea_new(
        f, mp_obj_get_int(args[0]), mp_obj_get_int(args[1]),
        mp_obj_get_int(args[2]), mp_obj_get_int(args[3]), c));
    o->img_ref = fref;
    o->is_input = true;
    surf_node_set_on_touch(o->node, ti_touch, o);
    return MP_OBJ_FROM_PTR(o);
}
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(mod_textarea_obj, 4, 6,
                                           mod_textarea);

/* ---- Font (runtime textgrid font) ---- */

static mp_obj_t font_destroy(mp_obj_t self_in)
{
    surfer_font_obj_t *o = MP_OBJ_TO_PTR(self_in);
    if (o->font) {
        if (o->owned)
            surf_font_free(o->font);
        o->font = NULL;
    }
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_1(font_destroy_obj, font_destroy);

/* .codepoints() -> [cp, ...], ascending — every glyph this face carries.
 *
 * The bake range is a build-time decision (see tools/fontbake.c) and the
 * atlas is the only record of what it was, so without this there is no
 * way to ask a font what it can draw: a missing glyph renders as '?' and
 * is indistinguishable from a font that genuinely has '?' there. That
 * makes a font browser impossible to write and a missing-glyph bug
 * impossible to see, which is why this exists.
 *
 * The table is already sorted ascending (fontbake insertion-sorts it,
 * because the runtime binary-searches), so the list comes out in order
 * with no work here. */
static mp_obj_t font_codepoints(mp_obj_t self_in)
{
    surfer_font_obj_t *o = MP_OBJ_TO_PTR(self_in);
    if (!o->font)
        return mp_obj_new_list(0, NULL);
    mp_obj_t l = mp_obj_new_list(0, NULL);
    for (int32_t i = 0; i < o->font->nglyphs; i++)
        mp_obj_list_append(l, MP_OBJ_NEW_SMALL_INT(o->font->glyphs[i].cp));
    return l;
}
static MP_DEFINE_CONST_FUN_OBJ_1(font_codepoints_obj, font_codepoints);

static const mp_rom_map_elem_t font_locals_table[] = {
    {MP_ROM_QSTR(MP_QSTR_destroy), MP_ROM_PTR(&font_destroy_obj)},
    {MP_ROM_QSTR(MP_QSTR_codepoints), MP_ROM_PTR(&font_codepoints_obj)},
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
 * custom console font, which anchors it (img_ref) for as long as the
 * grid lives. Dropping the last reference to one nobody draws with does
 * NOT free the atlas — only Font.destroy() does — so it leaks rather
 * than dangles, which is what makes it safe to leave unrooted. */
static mp_obj_t mod_font(mp_obj_t data_in)
{
    /* a name selects a built-in; bytes decode a fontbake blob */
    if (mp_obj_is_str(data_in)) {
        const surf_font *b = surf_font_builtin(mp_obj_str_get_str(data_in));
        if (!b)
            mp_raise_ValueError(MP_ERROR_TEXT("no such font"));
        surfer_font_obj_t *bo = mp_obj_malloc(surfer_font_obj_t, &surfer_font_type);
        bo->font = (surf_font *)b;
        bo->owned = false;
        return MP_OBJ_FROM_PTR(bo);
    }
    mp_buffer_info_t buf;
    mp_get_buffer_raise(data_in, &buf, MP_BUFFER_READ);
    surf_font *f = surf_font_from_blob(buf.buf, buf.len);
    if (!f)
        mp_raise_ValueError(MP_ERROR_TEXT("bad font blob"));
    surfer_port_prepare_image(&f->atlas);   /* device DMA coherence */
    surfer_font_obj_t *o = mp_obj_malloc(surfer_font_obj_t, &surfer_font_type);
    o->font = f;
    o->owned = true;
    return MP_OBJ_FROM_PTR(o);
}
static MP_DEFINE_CONST_FUN_OBJ_1(mod_font_obj, mod_font);

/* surfer.fonts() -> list of built-in font names. mono=True filters to the
 * ones a textgrid will accept. */
static mp_obj_t mod_fonts(size_t n_args, const mp_obj_t *args)
{
    bool mono_only = n_args > 0 && mp_obj_is_true(args[0]);
    mp_obj_t list = mp_obj_new_list(0, NULL);
    for (int i = 0; i < surf_font_builtin_count(); i++) {
        const surf_font *f = surf_font_builtin_at(i);
        /* An emoji set is not a face anybody picks — it is the fallback
         * every face already points at, and it would pass the mono test
         * (one box per picture) and then be offered as a console font. */
        if (surf_font_is_color(f))
            continue;
        if (mono_only && !surf_font_is_mono(f))
            continue;
        mp_obj_list_append(list, mp_obj_new_str(surf_font_builtin_name(i),
                                                strlen(surf_font_builtin_name(i))));
    }
    return list;
}
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(mod_fonts_obj, 0, 1, mod_fonts);

/* surfer.emoji(name) -> the CHARACTER, or None if the set has not got it.
 * surfer.emoji()     -> every name in the set.
 *
 * A character rather than a codepoint, because what a caller wants is to
 * put it in a string: `surfer.label("build " + surfer.emoji("check"), ...)`.
 * `"\U0001F525"` is the same glyph and always works — MicroPython has real
 * unicode strings on every target here — so this is for readability, not
 * because the escape is unavailable.
 *
 * None rather than a placeholder for a name that is not in the set: a
 * wrong picture is worse than a visible gap, and it is what makes the
 * miss show up where the typo is. */
static mp_obj_t mod_emoji(size_t n_args, const mp_obj_t *args)
{
    if (!n_args) {
        mp_obj_t list = mp_obj_new_list(0, NULL);
        for (int i = 0; i < surf_emoji_count(); i++) {
            const char *n = surf_emoji_name_at(i);
            mp_obj_list_append(list, mp_obj_new_str(n, strlen(n)));
        }
        return list;
    }
    uint32_t cp = surf_emoji_cp(mp_obj_str_get_str(args[0]));
    if (!cp)
        return mp_const_none;
    /* encode the one codepoint as UTF-8; mp_obj_new_str wants bytes */
    char b[5];
    int i = 0;
    if (cp < 0x80) {
        b[i++] = (char)cp;
    } else if (cp < 0x800) {
        b[i++] = (char)(0xc0 | (cp >> 6));
        b[i++] = (char)(0x80 | (cp & 0x3f));
    } else if (cp < 0x10000) {
        b[i++] = (char)(0xe0 | (cp >> 12));
        b[i++] = (char)(0x80 | ((cp >> 6) & 0x3f));
        b[i++] = (char)(0x80 | (cp & 0x3f));
    } else {
        b[i++] = (char)(0xf0 | (cp >> 18));
        b[i++] = (char)(0x80 | ((cp >> 12) & 0x3f));
        b[i++] = (char)(0x80 | ((cp >> 6) & 0x3f));
        b[i++] = (char)(0x80 | (cp & 0x3f));
    }
    return mp_obj_new_str(b, (size_t)i);
}
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(mod_emoji_obj, 0, 1, mod_emoji);

static mp_obj_t mod_textgrid(size_t n_args, const mp_obj_t *args)
{
    surf_color fg = n_args > 2 ? (surf_color)mp_obj_get_int(args[2])
                               : SURF_RGB(200, 205, 215);
    surf_color bg = n_args > 3 ? (surf_color)mp_obj_get_int(args[3])
                               : SURF_RGB(18, 20, 25);
    mp_obj_t font_ref = mp_const_none;
    const surf_font *f = n_args > 4 ? font_arg(args[4], &font_ref)
                                    : surf_font_builtin("mono16");
    /* the grid sizes its cell from 'M'; a proportional face would have
     * every wider glyph clipped, so refuse it outright */
    if (surf_font_is_color(f))
        mp_raise_ValueError(MP_ERROR_TEXT("that is an emoji set, not a font"));
    if (!surf_font_is_mono(f))
        mp_raise_ValueError(MP_ERROR_TEXT("textgrid needs a monospace font"));
    surfer_node_obj_t *o = new_node_obj(surf_textgrid_new(
        f, mp_obj_get_int(args[0]), mp_obj_get_int(args[1]), fg, bg));
    if (o)
        o->img_ref = font_ref;
    return MP_OBJ_FROM_PTR(o);
}
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(mod_textgrid_obj, 2, 5, mod_textgrid);

/* surfer.scrollbar(x, y, len, vertical=True) -> Widget.
 * .set_range(total, visible, pos) in whatever unit you like; .value is
 * the position, and .callback fires with a new one when it is dragged. */
static mp_obj_t mod_scrollbar(size_t n_args, const mp_obj_t *args)
{
    static const surf_scrollbar_style st = {
        .thumb = &sbar_img, .track = &sbtrack_img, .inset = WSBAR_INSET,
        .thumb_h = &sbarh_img, .track_h = &sbtrackh_img,
    };
    bool vertical = n_args > 3 ? mp_obj_is_true(args[3]) : true;
    surf_scrollbar *sb = surf_scrollbar_new(surf_screen(), 0, 0,
                                            (int16_t)mp_obj_get_int(args[2]),
                                            vertical, &st);
    if (!sb)
        mp_raise_msg(&mp_type_RuntimeError, MP_ERROR_TEXT("scrollbar create failed"));
    surf_node *node = surf_scrollbar_node(sb);
    surf_node_detach(node);           /* caller parents it via .add() */
    surf_node_set_pos(node, (int16_t)mp_obj_get_int(args[0]),
                      (int16_t)mp_obj_get_int(args[1]));
    surfer_widget_obj_t *o = new_widget_obj(W_SCROLLBAR, sb, node);
    surf_scrollbar_on_change(sb, widget_cb, o);
    return MP_OBJ_FROM_PTR(o);
}
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(mod_scrollbar_obj, 3, 4, mod_scrollbar);

static mp_obj_t mod_scrollview(size_t n_args, const mp_obj_t *args)
{
    (void)n_args;
    return MP_OBJ_FROM_PTR(new_node_obj(surf_scrollview_new(
        mp_obj_get_int(args[0]), mp_obj_get_int(args[1]),
        mp_obj_get_int(args[2]), mp_obj_get_int(args[3]))));
}
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(mod_scrollview_obj, 4, 4, mod_scrollview);

/* surfer.led(x, y, color) -> Widget. An indicator, not a control: it has
 * no callback. .value takes True/False or a brightness 0..1, and .color
 * is settable — the art is A8, so a retint costs a repaint and no
 * pixels. */
/* surfer.widget_font([name_or_font]) -> the current one, by NAME.
 *
 * Buttons and dropdowns draw their labels in this. It applies to widgets
 * created AFTER the call — a button builds its label node at
 * construction, so nothing already on screen changes face under it — and
 * with no argument it just reports what is in force.
 *
 * Set from a Font object it reports None, because a surf_font carries no
 * name and there is nowhere to keep the object: this state is a C static
 * that outlives the VM, so parking a Python object here is how you hand
 * back one the GC freed a soft reset ago. Nothing is lost that surfer
 * knew — the caller has the object it passed in. */
static mp_obj_t mod_widget_font(size_t n_args, const mp_obj_t *args)
{
    if (n_args) {
        widget_font_cache = font_arg(args[0], NULL);
        if (mp_obj_is_str(args[0])) {
            size_t len;
            const char *s = mp_obj_str_get_data(args[0], &len);
            if (len >= sizeof(widget_font_name))
                len = sizeof(widget_font_name) - 1;
            memcpy(widget_font_name, s, len);
            widget_font_name[len] = '\0';
            widget_font_unnamed = false;
        } else {
            widget_font_name[0] = '\0';
            widget_font_unnamed = true;
        }
    }
    if (widget_font_unnamed)
        return mp_const_none;
    const char *name = widget_font_name[0] ? widget_font_name : WIDGET_FONT;
    return mp_obj_new_str(name, strlen(name));
}
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(mod_widget_font_obj, 0, 1,
                                           mod_widget_font);

static mp_obj_t mod_led(size_t n_args, const mp_obj_t *args)
{
    surf_led_style st = {
        .strip = &led_img, .frame_w = WLED_SIZE, .frame_h = WLED_SIZE,
        .frames = WLED_FRAMES,
        .color = n_args > 2 ? (surf_color)mp_obj_get_int(args[2])
                            : SURF_RGB(255, 60, 40),   /* panel-lamp red */
    };
    surf_led *l = surf_led_new(surf_screen(), 0, 0, &st);
    if (!l)
        mp_raise_msg(&mp_type_RuntimeError, MP_ERROR_TEXT("led create failed"));
    surf_node *node = surf_led_node(l);
    surf_node_detach(node);           /* caller parents it via .add() */
    surf_node_set_pos(node, (int16_t)mp_obj_get_int(args[0]),
                      (int16_t)mp_obj_get_int(args[1]));
    return MP_OBJ_FROM_PTR(new_widget_obj(W_LED, l, node));
}
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(mod_led_obj, 2, 3, mod_led);

/* surfer.selector(x, y, positions) -> Widget. A knob with detents:
 * .value is an INDEX, the callback reports an index, a drag snaps and a
 * tap advances one position. */
static mp_obj_t mod_selector(mp_obj_t x, mp_obj_t y, mp_obj_t positions)
{
    /* the strip is baked on the first selector of the session; a widget
     * copies the struct and points at the shared pixels */
    surf_knob_style st = {.strip = surf_art_selector_strip(WSEL_SIZE),
                          .frame_w = WSEL_SIZE, .frame_h = WSEL_SIZE,
                          .frames = SURF_ART_FRAMES};
    surf_selector *sel = st.strip ? surf_selector_new(surf_screen(), 0, 0, &st,
                                                      mp_obj_get_int(positions))
                                  : NULL;
    if (!sel)
        mp_raise_msg(&mp_type_RuntimeError, MP_ERROR_TEXT("selector create failed"));
    surf_node *node = surf_selector_node(sel);
    surf_node_detach(node);
    surf_node_set_pos(node, (int16_t)mp_obj_get_int(x),
                      (int16_t)mp_obj_get_int(y));
    surfer_widget_obj_t *o = new_widget_obj(W_SELECTOR, sel, node);
    surf_selector_on_change(sel, widget_idx_cb, o);
    return MP_OBJ_FROM_PTR(o);
}
static MP_DEFINE_CONST_FUN_OBJ_3(mod_selector_obj, mod_selector);

/* surfer.colorpicker(x, y, size) -> Widget. .value is a packed colour
 * (what surfer.rgb() returns), settable, and the callback reports one. */
static mp_obj_t mod_colorpicker(mp_obj_t x, mp_obj_t y, mp_obj_t size)
{
    surf_colorpicker *c = surf_colorpicker_new(surf_screen(), 0, 0,
                                               (int16_t)mp_obj_get_int(size));
    if (!c)
        mp_raise_msg(&mp_type_RuntimeError,
                     MP_ERROR_TEXT("colorpicker create failed"));
    surf_node *node = surf_colorpicker_node(c);
    surf_node_detach(node);
    surf_node_set_pos(node, (int16_t)mp_obj_get_int(x),
                      (int16_t)mp_obj_get_int(y));
    surfer_widget_obj_t *o = new_widget_obj(W_COLORPICKER, c, node);
    surf_colorpicker_on_change(c, widget_idx_cb, o);
    return MP_OBJ_FROM_PTR(o);
}
static MP_DEFINE_CONST_FUN_OBJ_3(mod_colorpicker_obj, mod_colorpicker);

static mp_obj_t mod_slider(size_t n_args, const mp_obj_t *args)
{
    static const surf_slider_style st = {.track = &track_img,
                                         .inset = WTRACK_INSET, .cap = &cap_img,
                                         .track_h = &trackh_img,
                                         .cap_h = &caph_img};
    static const surf_slider_style slim = {.track = &slimtrack_img,
                                           .inset = WSLIMTRACK_INSET,
                                           .cap = &slimcap_img,
                                           .track_h = &slimtrackh_img,
                                           .cap_h = &slimcaph_img};
    /* surfer.slider(x, y, w, h) — the SHAPE picks the orientation, so
     * slider(x, y, 240, 40) is a horizontal one and needs no flag. */
    int16_t w = n_args > 2 ? (int16_t)mp_obj_get_int(args[2]) : WTRACKFULL_W;
    int16_t h = n_args > 3 ? (int16_t)mp_obj_get_int(args[3]) : WTRACKFULL_H;
    /* ...and the SHAPE picks the art too, by the same argument: a caller
     * asking for a slider 24px across has asked for the compact one and
     * should not have to say so twice. The threshold is the full cap's
     * own width, so the rule reads "narrower than the fader cap fits".
     * It is also the difference between working and not — a 24-wide
     * slider is smaller than that cap, and surf_slider_new refuses it. */
    const surf_slider_style *use = (w > h ? h : w) < WCAP_W ? &slim : &st;
    surf_slider *s = surf_slider_new(surf_screen(), 0, 0, w, h, use);
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
    /* third arg: pixel size — anything < 52 gets the small style. The
     * strip for that size is baked on the first knob of the session and
     * shared by every one after it (src/widgets/art.c). */
    int16_t size = (n_args > 2 && mp_obj_get_int(args[2]) < 52) ? WKNOBSM_SIZE
                                                                 : WKNOB_SIZE;
    surf_knob_style st = {.strip = surf_art_knob_strip(size), .frame_w = size,
                          .frame_h = size, .frames = SURF_ART_FRAMES};
    surf_knob *k = st.strip ? surf_knob_new(surf_screen(), 0, 0, &st) : NULL;
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
    st.font = widget_font();
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

/* surfer.tabs(x, y, w, h, ["one", "two"], tab_h=36, face=None, dim=None)
 *
 * A strip of TABS with a page behind each: `t.page(i)` is a group to
 * fill and the widget hides all but the current one. `.value` is the
 * index, settable, and the callback reports one.
 *
 * `face` is the current tab's colour and is meant to be THE PAGE'S
 * BACKGROUND — that is what makes the join disappear and a tab read as
 * a tab rather than a button near a panel. `dim` is every other tab.
 * Both default to surfer's own panel greys.
 *
 * `h` is the WHOLE height, tab strip included — a page gets h - tab_h,
 * which is what a caller laying out a panel actually knows. */
static mp_obj_t mod_tabs(size_t n_args, const mp_obj_t *args)
{
    static surf_tabs_style st = {
        .patch = &tab_img, .inset_side = WTAB_INSET_SIDE,
        .inset_top = WTAB_INSET_TOP, .inset_bottom = WTAB_INSET_BOT,
        .text_active = SURF_RGB(255, 255, 255),
        .text = SURF_RGB(150, 156, 172),
    };
    st.font = widget_font();
    st.font_active = widget_font();
    st.face = n_args > 6 ? (surf_color)mp_obj_get_int(args[6])
                         : SURF_RGB(47, 51, 62);
    st.dim = n_args > 7 ? (surf_color)mp_obj_get_int(args[7])
                        : SURF_RGB(28, 31, 38);
    /* The legend colours, for a caller whose face is LIGHT: the white
     * default disappears on a pale page, and the style already carries
     * both fields -- this only hands them to Python. */
    st.text = n_args > 8 ? (surf_color)mp_obj_get_int(args[8])
                         : SURF_RGB(150, 156, 172);
    st.text_active = n_args > 9 ? (surf_color)mp_obj_get_int(args[9])
                                : SURF_RGB(255, 255, 255);
    size_t len;
    mp_obj_t *items;
    mp_obj_get_array(args[4], &len, &items);
    if (len == 0)
        mp_raise_ValueError(MP_ERROR_TEXT("no tabs"));
    const char **strs = m_new(const char *, len);
    for (size_t i = 0; i < len; i++)
        strs[i] = mp_obj_str_get_str(items[i]);
    int16_t tab_h = n_args > 5 ? (int16_t)mp_obj_get_int(args[5]) : 36;
    surf_tabs *t = surf_tabs_new(surf_screen(), 0, 0,
                                 (int16_t)mp_obj_get_int(args[2]),
                                 (int16_t)mp_obj_get_int(args[3]), tab_h,
                                 &st, strs, (int32_t)len);
    m_del(const char *, strs, len);
    if (!t)
        mp_raise_msg(&mp_type_RuntimeError, MP_ERROR_TEXT("tabs create failed"));
    surf_node *node = surf_tabs_node(t);
    surf_node_detach(node);
    surf_node_set_pos(node, (int16_t)mp_obj_get_int(args[0]),
                      (int16_t)mp_obj_get_int(args[1]));
    surfer_widget_obj_t *o = new_widget_obj(W_TABS, t, node);
    surf_tabs_on_change(t, widget_idx_cb, o);
    return MP_OBJ_FROM_PTR(o);
}
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(mod_tabs_obj, 5, 10, mod_tabs);

/* surfer.radio(x, y, ["one", "two"], vertical=True) -> Widget
 *
 * N options, exactly one chosen. `.value` is the index, settable, and
 * the callback reports one — the checkbox's sibling, and the thing to
 * reach for when three checkboxes would be a lie about the choice.
 *
 * A ROW (vertical=False) is as wide as its labels, so ask the node what
 * it measured (`w.node.w`) rather than assuming a pitch. */
static mp_obj_t mod_radio(size_t n_args, const mp_obj_t *args)
{
    static surf_radio_style st = {
        .strip = &radio_img, .frame_w = WRADIO_SIZE, .frame_h = WRADIO_SIZE,
        .text_color = SURF_RGB(228, 232, 240), .gap = 10,
    };
    st.font = widget_font();
    size_t len;
    mp_obj_t *items;
    mp_obj_get_array(args[2], &len, &items);
    if (len == 0)
        mp_raise_ValueError(MP_ERROR_TEXT("no options"));
    const char **strs = m_new(const char *, len);
    for (size_t i = 0; i < len; i++)
        strs[i] = mp_obj_str_get_str(items[i]);
    bool vert = n_args > 3 ? mp_obj_is_true(args[3]) : true;
    surf_radio *r = surf_radio_new(surf_screen(), 0, 0, &st, strs,
                                   (int32_t)len, vert);
    m_del(const char *, strs, len);
    if (!r)
        mp_raise_msg(&mp_type_RuntimeError, MP_ERROR_TEXT("radio create failed"));
    surf_node *node = surf_radio_node(r);
    surf_node_detach(node);
    surf_node_set_pos(node, (int16_t)mp_obj_get_int(args[0]),
                      (int16_t)mp_obj_get_int(args[1]));
    surfer_widget_obj_t *o = new_widget_obj(W_RADIO, r, node);
    surf_radio_on_change(r, widget_idx_cb, o);
    return MP_OBJ_FROM_PTR(o);
}
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(mod_radio_obj, 3, 4, mod_radio);

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
    st.font = widget_font();
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
/* surfer._touch(x, y, phase[, id]) — push one touch through the normal
 * dispatch path. `id` is the CONTACT: capture is per contact, so a test
 * drives three fingers by using three ids. It defaults to 0, which is
 * what a mouse is and what every existing caller means. */
static mp_obj_t mod_touch(size_t n_args, const mp_obj_t *args)
{
    surf_touch t = {(int16_t)mp_obj_get_int(args[0]),
                    (int16_t)mp_obj_get_int(args[1]),
                    (uint8_t)mp_obj_get_int(args[2]),
                    (uint8_t)(n_args > 3 ? mp_obj_get_int(args[3]) : 0)};
    surf_inject_touch(&t);
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(mod_touch_obj, 3, 4, mod_touch);

/* surfer._key(kind, text="", shift=False, ctrl=False): push one key into
 * the same queue a driver feeds, so surfer.keys() returns it. The
 * counterpart of _touch — it exists for headless tests, which otherwise
 * cannot reach anything that reads the keyboard. `ctrl` is what lets one
 * test a chord: ctrl+LETTER is a control character a test can simply
 * type, but ctrl+Delete and ctrl+arrow exist ONLY as this flag. */
static mp_obj_t mod_key(size_t n_args, const mp_obj_t *args)
{
    surfer_key k = {.kind = (uint8_t)mp_obj_get_int(args[0]),
                    .shift = n_args > 2 && mp_obj_is_true(args[2]),
                    .ctrl = n_args > 3 && mp_obj_is_true(args[3])};
    if (n_args > 1) {
        const char *t = mp_obj_str_get_str(args[1]);
        size_t n = strlen(t);
        if (n > sizeof k.utf8 - 1)
            n = sizeof k.utf8 - 1;
        memcpy(k.utf8, t, n);
    }
    surf_key_event(&k);
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(mod_key_obj, 1, 4, mod_key);

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
    {MP_ROM_QSTR(MP_QSTR_wheel), MP_ROM_PTR(&mod_wheel_obj)},
    {MP_ROM_QSTR(MP_QSTR_screen_keyboard), MP_ROM_PTR(&mod_screen_keyboard_obj)},
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
    {MP_ROM_QSTR(MP_QSTR_image_scale), MP_ROM_PTR(&mod_image_scale_obj)},
    {MP_ROM_QSTR(MP_QSTR_mesh), MP_ROM_PTR(&mod_mesh_obj)},
    {MP_ROM_QSTR(MP_QSTR_layer), MP_ROM_PTR(&mod_layer_obj)},
    {MP_ROM_QSTR(MP_QSTR_fb_read), MP_ROM_PTR(&mod_fb_read_obj)},
    {MP_ROM_QSTR(MP_QSTR_fb_image), MP_ROM_PTR(&mod_fb_image_obj)},
    {MP_ROM_QSTR(MP_QSTR_has_touch), MP_ROM_PTR(&mod_has_touch_obj)},
    {MP_ROM_QSTR(MP_QSTR__touch_info), MP_ROM_PTR(&mod_touch_info_obj)},
    {MP_ROM_QSTR(MP_QSTR_sprite), MP_ROM_PTR(&mod_sprite_obj)},
    {MP_ROM_QSTR(MP_QSTR_filmstrip), MP_ROM_PTR(&mod_filmstrip_obj)},
    {MP_ROM_QSTR(MP_QSTR_write_png), MP_ROM_PTR(&mod_write_png_obj)},
    {MP_ROM_QSTR(MP_QSTR_label), MP_ROM_PTR(&mod_label_obj)},
    {MP_ROM_QSTR(MP_QSTR_text_image), MP_ROM_PTR(&mod_text_image_obj)},
    {MP_ROM_QSTR(MP_QSTR_textinput), MP_ROM_PTR(&mod_textinput_obj)},
    {MP_ROM_QSTR(MP_QSTR_textarea), MP_ROM_PTR(&mod_textarea_obj)},
    {MP_ROM_QSTR(MP_QSTR_textgrid), MP_ROM_PTR(&mod_textgrid_obj)},
    {MP_ROM_QSTR(MP_QSTR_font), MP_ROM_PTR(&mod_font_obj)},
    {MP_ROM_QSTR(MP_QSTR_fonts), MP_ROM_PTR(&mod_fonts_obj)},
    {MP_ROM_QSTR(MP_QSTR_emoji), MP_ROM_PTR(&mod_emoji_obj)},
    {MP_ROM_QSTR(MP_QSTR_widget_font), MP_ROM_PTR(&mod_widget_font_obj)},
    {MP_ROM_QSTR(MP_QSTR_scrollview), MP_ROM_PTR(&mod_scrollview_obj)},
    {MP_ROM_QSTR(MP_QSTR_scrollbar), MP_ROM_PTR(&mod_scrollbar_obj)},
    {MP_ROM_QSTR(MP_QSTR_slider), MP_ROM_PTR(&mod_slider_obj)},
    {MP_ROM_QSTR(MP_QSTR_knob), MP_ROM_PTR(&mod_knob_obj)},
    {MP_ROM_QSTR(MP_QSTR_checkbox), MP_ROM_PTR(&mod_checkbox_obj)},
    {MP_ROM_QSTR(MP_QSTR_dropdown), MP_ROM_PTR(&mod_dropdown_obj)},
    {MP_ROM_QSTR(MP_QSTR_button), MP_ROM_PTR(&mod_button_obj)},
    {MP_ROM_QSTR(MP_QSTR_led), MP_ROM_PTR(&mod_led_obj)},
    {MP_ROM_QSTR(MP_QSTR_selector), MP_ROM_PTR(&mod_selector_obj)},
    {MP_ROM_QSTR(MP_QSTR_tabs), MP_ROM_PTR(&mod_tabs_obj)},
    {MP_ROM_QSTR(MP_QSTR_radio), MP_ROM_PTR(&mod_radio_obj)},
    {MP_ROM_QSTR(MP_QSTR_colorpicker), MP_ROM_PTR(&mod_colorpicker_obj)},
    /* capitalized aliases, DESIGN.md §3 taste */
    {MP_ROM_QSTR(MP_QSTR_Group), MP_ROM_PTR(&mod_group_obj)},
    {MP_ROM_QSTR(MP_QSTR_TextInput), MP_ROM_PTR(&mod_textinput_obj)},
    {MP_ROM_QSTR(MP_QSTR_Slider), MP_ROM_PTR(&mod_slider_obj)},
    {MP_ROM_QSTR(MP_QSTR_Knob), MP_ROM_PTR(&mod_knob_obj)},
    {MP_ROM_QSTR(MP_QSTR_Checkbox), MP_ROM_PTR(&mod_checkbox_obj)},
    {MP_ROM_QSTR(MP_QSTR_Dropdown), MP_ROM_PTR(&mod_dropdown_obj)},
    {MP_ROM_QSTR(MP_QSTR_Button), MP_ROM_PTR(&mod_button_obj)},
    {MP_ROM_QSTR(MP_QSTR_Led), MP_ROM_PTR(&mod_led_obj)},
    {MP_ROM_QSTR(MP_QSTR_Selector), MP_ROM_PTR(&mod_selector_obj)},
    {MP_ROM_QSTR(MP_QSTR_ColorPicker), MP_ROM_PTR(&mod_colorpicker_obj)},
    {MP_ROM_QSTR(MP_QSTR__touch), MP_ROM_PTR(&mod_touch_obj)},
    {MP_ROM_QSTR(MP_QSTR__key), MP_ROM_PTR(&mod_key_obj)},
    {MP_ROM_QSTR(MP_QSTR__wheel), MP_ROM_PTR(&mod_wheel_inject_obj)},
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
    {MP_ROM_QSTR(MP_QSTR_KEY_ESC), MP_ROM_INT(12)},
    /* fonts */
    /* NAMES, not registry indices: index 0/1/2 is whatever the Makefile
     * lists first, and after the registry grew these three resolved to
     * ui12, ui16 and ui16b -- a proportional face textgrid refuses. Every
     * font argument takes a name, so the constant IS the name. */
    {MP_ROM_QSTR(MP_QSTR_FONT_UI16), MP_ROM_QSTR(MP_QSTR_ui16)},
    {MP_ROM_QSTR(MP_QSTR_FONT_UI28), MP_ROM_QSTR(MP_QSTR_ui28)},
    {MP_ROM_QSTR(MP_QSTR_FONT_MONO16), MP_ROM_QSTR(MP_QSTR_mono16)},
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
