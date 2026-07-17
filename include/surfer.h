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
} surf_format;

typedef struct {
    void    *pixels;
    int16_t  w, h;
    int32_t  stride;  /* bytes per row; must be a 64-byte multiple on device */
    uint8_t  format;  /* surf_format */
    bool     opaque;  /* no alpha content: compositor may use blit, not blend */
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
} surf_hal;

typedef struct surf_node surf_node;

/* Widget values are Q16 fixed point in [0, SURF_ONE]. */
#define SURF_ONE 65536

typedef void (*surf_touch_cb)(surf_node *n, const surf_touch *t, void *user);
typedef void (*surf_change_cb)(int32_t value_q16, void *user);

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
