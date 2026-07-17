/* Core internals — not part of the binding surface. */
#ifndef SURF_INTERNAL_H
#define SURF_INTERNAL_H

#include "surfer.h"

#define SURF_MAX_DIRTY 16

enum {
    SURF_NODE_FREE = 0,
    SURF_NODE_GROUP,
    SURF_NODE_RECT,
    SURF_NODE_SPRITE,
    SURF_NODE_FILMSTRIP,
    SURF_NODE_NINEPATCH,
};

enum {
    SURF_NF_HIDDEN = 1u << 0,
    SURF_NF_CLIP   = 1u << 1,
};

struct surf_node {
    uint8_t    type;
    uint8_t    flags;
    int16_t    x, y;   /* offset in parent */
    int16_t    w, h;   /* rect: size; sprite: src size; group: clip size */
    surf_node *parent;
    surf_node *first, *last;  /* children; last is painted frontmost */
    surf_node *prev, *next;   /* siblings; next doubles as free-list link */
    surf_touch_cb on_touch;
    void      *touch_user;
    union {
        struct { surf_color color; } rect;
        struct { const surf_image *img; surf_rect src; } sprite;
        struct {
            const surf_image *img;
            int16_t fw, fh;      /* frame size; node w/h mirror these */
            int16_t frame, nframes, per_row;
        } strip;
        struct {
            const surf_image *img;
            int16_t l, t, r, b;  /* insets; node w/h are the dst size */
        } nine;
    } u;
};

/* rect ops */
static inline bool surf_rect_empty(surf_rect r) { return r.w <= 0 || r.h <= 0; }
surf_rect surf_rect_intersect(surf_rect a, surf_rect b);
surf_rect surf_rect_union(surf_rect a, surf_rect b);
bool      surf_rect_overlaps(surf_rect a, surf_rect b);
bool      surf_rect_covers(surf_rect a, surf_rect b);  /* a covers all of b */
bool      surf_rect_contains(surf_rect r, int16_t x, int16_t y);

/* dirty-rect list: merge on overlap, degrade to bounding union at the cap */
typedef struct {
    surf_rect r[SURF_MAX_DIRTY];
    int       n;
    surf_rect clip;  /* screen bounds; adds are clipped to this */
} surf_dirty;

void surf_dirty_reset(surf_dirty *d, surf_rect clip);
void surf_dirty_add(surf_dirty *d, surf_rect r);

/* per-dirty-rect paint list, filled front-to-back, painted in reverse */
typedef struct {
    surf_node *n;
    int16_t    ax, ay;  /* absolute position */
    surf_rect  vis;     /* visible part, pre-clipped */
} surf_paint_ent;

typedef struct {
    const surf_hal *hal;
    int16_t         w, h;
    surf_color      bg;
    surf_node      *pool;
    int             pool_cap;
    surf_node      *free_list;
    surf_node      *root;
    surf_node      *capture;  /* node holding the pointer, DOWN → UP */
    surf_dirty      dirty;
    surf_paint_ent *plist;  /* pool_cap entries */
} surf_ctx;

extern surf_ctx surf_g;

void surf_input_dispatch(const surf_touch *t);

bool      surf_node_attached(const surf_node *n);
surf_rect surf_node_subtree_bounds(const surf_node *n, int16_t px, int16_t py);
void      surf_damage_subtree(const surf_node *n);
void      surf_compose(void);  /* compose all dirty rects + present */

#endif /* SURF_INTERNAL_H */
