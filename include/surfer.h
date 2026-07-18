/* surfer — public API. This header is the whole binding surface; keep it
 * flat and boring (see DESIGN.md §3). */
#ifndef SURFER_H
#define SURFER_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct { int16_t x, y, w, h; } surf_rect;
typedef struct { int16_t x, y; } surf_point;

/* RGB565 everywhere the framebuffer is concerned (DESIGN.md §5.1). */
typedef uint16_t surf_color;
#define SURF_RGB(r, g, b) \
    ((surf_color)((((r) & 0xf8) << 8) | (((g) & 0xfc) << 3) | (((b) & 0xf8) >> 3)))

typedef enum {
    SURF_FMT_RGB565   = 0,
    SURF_FMT_ARGB8888 = 1,
    SURF_FMT_A8       = 2,  /* alpha-only (glyph atlases); tinted on blend */
} surf_format;

typedef struct {
    void      *pixels;
    int16_t    w, h;
    int32_t    stride;  /* bytes per row; must be a 64-byte multiple on device */
    uint8_t    format;  /* surf_format */
    bool       opaque;  /* no alpha content: compositor may use blit, not blend */
    surf_color tint;    /* A8 only: the color the alpha modulates */
} surf_image;

typedef enum {
    SURF_TOUCH_DOWN = 0,
    SURF_TOUCH_MOVE = 1,
    SURF_TOUCH_UP   = 2,
} surf_touch_phase;

typedef struct { int16_t x, y; uint8_t phase; } surf_touch;

/* The hal vtable — the only thing a backend implements (DESIGN.md §2.1). */
typedef struct {
    void (*fill)(surf_rect dst, surf_color c);
    void (*blit)(const surf_image *src, surf_rect src_r, surf_point dst);
    void (*blend)(const surf_image *src, surf_rect src_r, surf_point dst, uint8_t opa);
    void (*scale_blit)(const surf_image *src, surf_rect src_r, surf_rect dst_r);
    void (*present)(const surf_rect *dirty, int n);
    void (*wait_idle)(void);
    uint64_t (*now_us)(void);
    bool (*poll_touch)(surf_touch *out);
    void *(*alloc_image)(size_t bytes);  /* 64-byte aligned */
    void (*free_image)(void *p);
    /* Optional (may be NULL): CPU pointer to the current RGB565 compose
     * target. Exists for exactly one caller — the textgrid fast path —
     * because the PPA's ~85µs-per-op floor makes per-glyph blits ~15×
     * too slow for full-screen text (see DESIGN.md §5.6). The hal owns
     * any cache sync of CPU-written regions at present time. */
    void *(*fb_ptr)(int32_t *stride_bytes);
} surf_hal;

typedef struct surf_node surf_node;

/* Widget values are Q16 fixed point in [0, SURF_ONE]. */
#define SURF_ONE 65536

typedef void (*surf_touch_cb)(surf_node *n, const surf_touch *t, void *user);
typedef void (*surf_change_cb)(int32_t value_q16, void *user);
typedef void (*surf_index_cb)(int32_t index, void *user);

typedef struct {
    int        max_nodes;  /* node pool size; 0 → 256 */
    surf_color bg;         /* fill color under non-opaque content */
} surf_config;

/* lifecycle */
bool       surf_init(const surf_hal *hal, int16_t w, int16_t h, const surf_config *cfg);
void       surf_deinit(void);
surf_node *surf_screen(void);
void       surf_tick(void);  /* compose dirty rects + present */

/* node constructors (from the pool; NULL when exhausted) */
surf_node *surf_group_new(int16_t x, int16_t y);
surf_node *surf_rect_new(int16_t x, int16_t y, int16_t w, int16_t h, surf_color c);
surf_node *surf_sprite_new(const surf_image *img, int16_t x, int16_t y);
surf_node *surf_filmstrip_new(const surf_image *img, int16_t frame_w, int16_t frame_h,
                              int16_t x, int16_t y);
surf_node *surf_ninepatch_new(const surf_image *img, int16_t x, int16_t y,
                              int16_t w, int16_t h,
                              int16_t l, int16_t t, int16_t r, int16_t b);

/* scrollview: a clipped group whose children draw at a content offset.
 * Dragging inside it scrolls after an 8px threshold steals the gesture
 * from child handlers (widgets that own drags — sliders, knobs — opt out
 * via surf_node_set_gesture_grab). Momentum and edge spring-back run in
 * surf_tick, in core, so every backend feels identical (DESIGN.md §2.6). */
surf_node *surf_scrollview_new(int16_t x, int16_t y, int16_t w, int16_t h);
void       surf_scrollview_set_offset(surf_node *sv, int16_t x, int16_t y);
surf_point surf_scrollview_offset(const surf_node *sv);
surf_point surf_scrollview_content_size(surf_node *sv);

/* tree — detach keeps the subtree fully alive (DESIGN.md §2.2) */
void surf_node_add(surf_node *parent, surf_node *child);
void surf_node_detach(surf_node *child);
void surf_node_destroy(surf_node *n);  /* detaches, then frees the subtree */

/* properties — every write damages the old and new screen rects */
void surf_node_set_pos(surf_node *n, int16_t x, int16_t y);
void surf_node_set_hidden(surf_node *n, bool hidden);
void surf_rect_set_color(surf_node *n, surf_color c);
void surf_rect_set_size(surf_node *n, int16_t w, int16_t h);
void surf_sprite_set_src(surf_node *n, surf_rect src);
void surf_group_set_clip(surf_node *g, int16_t w, int16_t h);  /* 0×0 disables */
void surf_filmstrip_set_frame(surf_node *n, int16_t frame);
int16_t surf_filmstrip_frame(const surf_node *n);
void surf_ninepatch_set_size(surf_node *n, int16_t w, int16_t h);

/* input: touch routes to the hit node's nearest ancestor with a handler,
 * which holds pointer capture until UP (DESIGN.md §2.6) */
surf_node *surf_hit_test(int16_t x, int16_t y);
void surf_node_set_on_touch(surf_node *n, surf_touch_cb cb, void *user);
void surf_node_abs_pos(const surf_node *n, int16_t *x, int16_t *y);
surf_point surf_node_pos(const surf_node *n);
surf_point surf_node_size(const surf_node *n);
/* decode the first codepoint of a UTF-8 string (0 for empty/NULL) */
uint32_t surf_utf8_first(const char *s);
/* grab = an enclosing scrollview may not steal this node's gestures */
void surf_node_set_gesture_grab(surf_node *n, bool grab);
/* feed a synthetic touch through the normal dispatch path (tests, OSK) */
void surf_inject_touch(const surf_touch *t);

/* ---- text: atlases baked at build time by tools/fontbake.c ---- */

typedef struct {
    uint32_t cp;
    int16_t  x, y, w, h;   /* atlas rect */
    int16_t  xoff, yoff;   /* bearing from the pen position */
    int16_t  adv;
} surf_glyph;

typedef struct { uint32_t a, b; int16_t adv; } surf_kern;

typedef struct {
    surf_image        atlas;   /* SURF_FMT_A8 */
    int16_t           ascent, descent, line_gap;  /* px; descent ≤ 0 */
    const surf_glyph *glyphs;  /* sorted by codepoint */
    int32_t           nglyphs;
    const surf_kern  *kerns;   /* sorted by (a, b); only non-zero pairs */
    int32_t           nkerns;
} surf_font;

#define surf_font_line_h(f) ((int16_t)((f)->ascent - (f)->descent + (f)->line_gap))

typedef enum {
    SURF_ALIGN_LEFT   = 0,
    SURF_ALIGN_CENTER = 1,
    SURF_ALIGN_RIGHT  = 2,
} surf_align;

surf_point surf_text_measure(const surf_font *f, const char *str, int16_t wrap_w);

/* label node: wrap at wrap_w (0 = single line), greedy break on space and
 * hyphen; ellipsize truncates a single line with U+2026 instead */
surf_node *surf_text_new(const surf_font *f, const char *str,
                         int16_t x, int16_t y, surf_color c);
void surf_text_set(surf_node *n, const char *str);
void surf_text_set_color(surf_node *n, surf_color c);
void surf_text_set_wrap(surf_node *n, int16_t wrap_w);
void surf_text_set_align(surf_node *n, surf_align a);
void surf_text_set_ellipsis(surf_node *n, bool on);

/* textinput node: single-line editable text + caret/selection state.
 * Indices are byte offsets into the UTF-8 buffer, always on a codepoint
 * boundary. The box/border art and the on-screen keyboard are widgets
 * built on top, not core (DESIGN.md §2.5). */
surf_node  *surf_textinput_new(const surf_font *f, int16_t x, int16_t y,
                               int16_t w, surf_color c);
void        surf_textinput_set_text(surf_node *n, const char *str);
const char *surf_textinput_text(const surf_node *n);
void        surf_textinput_insert(surf_node *n, const char *utf8);  /* at caret */
void        surf_textinput_backspace(surf_node *n);
void        surf_textinput_delete(surf_node *n);
int32_t     surf_textinput_caret(const surf_node *n);
void        surf_textinput_set_caret(surf_node *n, int32_t byte_idx, bool extend);
void        surf_textinput_move(surf_node *n, int32_t delta_cp, bool extend);
int32_t     surf_textinput_index_from_x(const surf_node *n, int16_t local_x);
void        surf_textinput_set_focused(surf_node *n, bool focused);

/* textgrid: the fast fixed-width text mode (terminals, code editors).
 * A cols×rows grid of opaque cells (codepoint + fg + bg), composed by
 * the CPU straight into the framebuffer — full-screen text scrolls at
 * frame rate where per-glyph blits cannot (DESIGN.md §5.6). Requires a
 * monospaced font; the cell is sized from 'M'. */
surf_node *surf_textgrid_new(const surf_font *f, int16_t cols, int16_t rows,
                             surf_color fg, surf_color bg);
void surf_textgrid_set_cell(surf_node *n, int16_t col, int16_t row, uint32_t cp,
                            surf_color fg, surf_color bg);
/* fill a row from UTF-8 in the default colors, space-padded to the edge */
void surf_textgrid_set_row(surf_node *n, int16_t row, const char *utf8);
/* positive = content moves up; exposed rows are blanked */
void surf_textgrid_scroll(surf_node *n, int16_t dy_rows);
surf_point surf_textgrid_cell_size(const surf_node *n);

typedef struct {
    const surf_image *strip;  /* 2 frames: unchecked, checked */
    int16_t           frame_w, frame_h;
} surf_checkbox_style;

typedef struct surf_checkbox surf_checkbox;

surf_checkbox *surf_checkbox_new(surf_node *parent, int16_t x, int16_t y,
                                 const surf_checkbox_style *style);
void       surf_checkbox_destroy(surf_checkbox *c);
surf_node *surf_checkbox_node(surf_checkbox *c);
bool       surf_checkbox_checked(const surf_checkbox *c);
void       surf_checkbox_set_checked(surf_checkbox *c, bool on);  /* no cb */
void       surf_checkbox_on_change(surf_checkbox *c, surf_change_cb cb, void *user);

typedef struct {
    const surf_image *normal;  /* 9-patch, unpressed */
    const surf_image *pressed;
    int16_t           inset;
    const surf_font  *font;
    surf_color        text_color;
} surf_button_style;

typedef struct surf_button surf_button;

surf_button *surf_button_new(surf_node *parent, int16_t x, int16_t y,
                             int16_t w, int16_t h, const surf_button_style *style,
                             const char *label);
void       surf_button_destroy(surf_button *b);
surf_node *surf_button_node(surf_button *b);
void       surf_button_set_label(surf_button *b, const char *label);
/* fires on release inside the button (value is always SURF_ONE) */
void       surf_button_on_press(surf_button *b, surf_change_cb cb, void *user);

typedef struct {
    const surf_image *panel;   /* 9-patch for the box and the popup */
    int16_t           inset;
    const surf_font  *font;
    surf_color        text_color, hi_color;
    const surf_image *arrow;   /* 2-frame strip: closed, open; NULL = none */
    int16_t           arrow_w, arrow_h;
} surf_dropdown_style;

typedef struct surf_dropdown surf_dropdown;

/* The open popup attaches to the screen root so it overlays siblings —
 * detach/reattach in action. Item strings are copied. */
surf_dropdown *surf_dropdown_new(surf_node *parent, int16_t x, int16_t y, int16_t w,
                                 const surf_dropdown_style *style,
                                 const char *const *items, int32_t nitems);
void       surf_dropdown_destroy(surf_dropdown *d);
surf_node *surf_dropdown_node(surf_dropdown *d);
int32_t    surf_dropdown_selected(const surf_dropdown *d);
void       surf_dropdown_set_selected(surf_dropdown *d, int32_t index);  /* no cb */
void       surf_dropdown_on_change(surf_dropdown *d, surf_index_cb cb, void *user);

/* ---- widgets: built from nodes, styled by caller-owned assets ---- */

typedef struct {
    const surf_image *track;  /* 9-patch source */
    int16_t           inset;  /* uniform 9-slice inset */
    const surf_image *cap;    /* cap sprite */
} surf_slider_style;

typedef struct surf_slider surf_slider;

surf_slider *surf_slider_new(surf_node *parent, int16_t x, int16_t y,
                             int16_t w, int16_t h, const surf_slider_style *style);
void       surf_slider_destroy(surf_slider *s);
surf_node *surf_slider_node(surf_slider *s);  /* root group: detach/reattach */
void       surf_slider_set_value(surf_slider *s, int32_t value_q16);  /* no cb */
int32_t    surf_slider_value(const surf_slider *s);
void       surf_slider_on_change(surf_slider *s, surf_change_cb cb, void *user);

typedef struct {
    const surf_image *strip;  /* filmstrip: frames left-to-right */
    int16_t           frame_w, frame_h;
    int16_t           frames;
} surf_knob_style;

typedef enum {
    SURF_KNOB_DRAG_VERTICAL = 0,  /* DAW convention (DESIGN.md §2.6) */
    SURF_KNOB_DRAG_ANGULAR  = 1,
} surf_knob_mode;

typedef struct surf_knob surf_knob;

surf_knob *surf_knob_new(surf_node *parent, int16_t x, int16_t y,
                         const surf_knob_style *style);
void       surf_knob_destroy(surf_knob *k);
surf_node *surf_knob_node(surf_knob *k);
void       surf_knob_set_mode(surf_knob *k, surf_knob_mode mode);
void       surf_knob_set_value(surf_knob *k, int32_t value_q16);  /* no cb */
int32_t    surf_knob_value(const surf_knob *k);
void       surf_knob_on_change(surf_knob *k, surf_change_cb cb, void *user);

#ifdef __cplusplus
}
#endif

#endif /* SURFER_H */
