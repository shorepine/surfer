#include <stdlib.h>
#include <string.h>

#include "surf_internal.h"

surf_ctx surf_g;

/* ---- pool ---- */

surf_node *surf_node_alloc(uint8_t type)
{
    surf_node *n = surf_g.free_list;
    if (!n)
        return NULL;
    surf_g.free_list = n->next;
    memset(n, 0, sizeof *n);
    n->type = type;
    return n;
}
#define node_alloc surf_node_alloc

static void node_free(surf_node *n)
{
    if (surf_g.capture == n)
        surf_g.capture = NULL;
    if (surf_g.steal_sv == n)
        surf_g.steal_sv = NULL;
    if (n->type == SURF_NODE_SCROLLVIEW)
        surf_scroll_forget(n);
    n->type = SURF_NODE_FREE;
    n->next = surf_g.free_list;
    surf_g.free_list = n;
}

/* ---- lifecycle ---- */

bool surf_init(const surf_hal *hal, int16_t w, int16_t h, const surf_config *cfg)
{
    if (!hal || w <= 0 || h <= 0)
        return false;

    memset(&surf_g, 0, sizeof surf_g);
    surf_g.hal = hal;
    surf_g.w = w;
    surf_g.h = h;
    surf_g.bg = cfg ? cfg->bg : 0;
    surf_g.pool_cap = (cfg && cfg->max_nodes > 0) ? cfg->max_nodes : 256;

    /* Pools are sized once here; the frame path never allocates. */
    surf_g.pool = calloc((size_t)surf_g.pool_cap, sizeof(surf_node));
    surf_g.plist = calloc((size_t)surf_g.pool_cap, sizeof(surf_paint_ent));
    if (!surf_g.pool || !surf_g.plist) {
        free(surf_g.pool);
        free(surf_g.plist);
        memset(&surf_g, 0, sizeof surf_g);
        return false;
    }
    for (int i = surf_g.pool_cap - 1; i >= 0; i--)
        node_free(&surf_g.pool[i]);

    surf_g.root = node_alloc(SURF_NODE_GROUP);
    surf_dirty_reset(&surf_g.dirty, (surf_rect){0, 0, w, h});
    surf_dirty_add(&surf_g.dirty, (surf_rect){0, 0, w, h});
    return true;
}

void surf_deinit(void)
{
    free(surf_g.pool);
    free(surf_g.plist);
    memset(&surf_g, 0, sizeof surf_g);
}

surf_node *surf_screen(void)
{
    return surf_g.root;
}

void surf_tick(void)
{
    if (!surf_g.hal)
        return;
    surf_touch t;
    while (surf_g.hal->poll_touch(&t))
        surf_input_dispatch(&t);
    surf_scroll_tick();  /* momentum + spring-back (DESIGN.md §2.3 step 1) */
    surf_compose();
}

/* ---- damage ---- */

bool surf_node_attached(const surf_node *n)
{
    while (n->parent)
        n = n->parent;
    return n == surf_g.root;
}

surf_rect surf_node_subtree_bounds(const surf_node *n, int16_t px, int16_t py)
{
    if (n->flags & SURF_NF_HIDDEN)
        return (surf_rect){0, 0, 0, 0};

    int16_t ax = (int16_t)(px + n->x), ay = (int16_t)(py + n->y);
    /* scrollview bounds are its viewport box — content is clipped inside */
    if (n->type != SURF_NODE_GROUP)
        return (surf_rect){ax, ay, n->w, n->h};

    surf_rect b = {0, 0, 0, 0};
    for (const surf_node *c = n->first; c; c = c->next)
        b = surf_rect_union(b, surf_node_subtree_bounds(c, ax, ay));
    if (n->flags & SURF_NF_CLIP)
        b = surf_rect_intersect(b, (surf_rect){ax, ay, n->w, n->h});
    return b;
}

/* Bounds walk up to the root, translating through each ancestor's offset —
 * minus its scroll offset when it's a scrollview — and clipping to any
 * clipped box on the way. Damage from deep inside a scrolled list lands on
 * exactly the visible pixels it affects, or nowhere. */
static surf_rect subtree_screen_rect(const surf_node *n)
{
    if (!surf_g.root || !surf_node_attached(n))
        return (surf_rect){0, 0, 0, 0};
    surf_rect b = surf_node_subtree_bounds(n, 0, 0);
    for (const surf_node *p = n->parent; p; p = p->parent) {
        if (surf_rect_empty(b))
            return b;
        if (p->type == SURF_NODE_SCROLLVIEW) {
            b.x = (int16_t)(b.x - (p->u.scroll.off_x >> 16));
            b.y = (int16_t)(b.y - (p->u.scroll.off_y >> 16));
            b = surf_rect_intersect(b, (surf_rect){0, 0, p->w, p->h});
        } else if (p->flags & SURF_NF_CLIP) {
            b = surf_rect_intersect(b, (surf_rect){0, 0, p->w, p->h});
        }
        b.x = (int16_t)(b.x + p->x);
        b.y = (int16_t)(b.y + p->y);
    }
    return b;
}

void surf_damage_subtree(const surf_node *n)
{
    surf_rect b = subtree_screen_rect(n);
    if (!surf_rect_empty(b))
        surf_dirty_add(&surf_g.dirty, b);
}

/* ---- constructors ---- */

surf_node *surf_group_new(int16_t x, int16_t y)
{
    surf_node *n = node_alloc(SURF_NODE_GROUP);
    if (n) { n->x = x; n->y = y; }
    return n;
}

surf_node *surf_rect_new(int16_t x, int16_t y, int16_t w, int16_t h, surf_color c)
{
    surf_node *n = node_alloc(SURF_NODE_RECT);
    if (n) {
        n->x = x; n->y = y; n->w = w; n->h = h;
        n->u.rect.color = c;
    }
    return n;
}

surf_node *surf_sprite_new(const surf_image *img, int16_t x, int16_t y)
{
    if (!img)
        return NULL;
    surf_node *n = node_alloc(SURF_NODE_SPRITE);
    if (n) {
        n->x = x; n->y = y; n->w = img->w; n->h = img->h;
        n->u.sprite.img = img;
        n->u.sprite.src = (surf_rect){0, 0, img->w, img->h};
        n->u.sprite.scale_q16 = SURF_ONE;
        n->u.sprite.rot = 0;
        n->u.sprite.mirror = 0;
    }
    return n;
}

/* node w/h = the on-screen footprint: src scaled, sides swapped for
 * quarter-turn rotations */
static void sprite_update_size(surf_node *n)
{
    int32_t w = (int32_t)(((int64_t)n->u.sprite.src.w * n->u.sprite.scale_q16) >> 16);
    int32_t h = (int32_t)(((int64_t)n->u.sprite.src.h * n->u.sprite.scale_q16) >> 16);
    if (w < 1) w = 1;
    if (h < 1) h = 1;
    if (n->u.sprite.rot & 1) {
        int32_t t = w; w = h; h = t;
    }
    n->w = (int16_t)w;
    n->h = (int16_t)h;
}

surf_node *surf_filmstrip_new(const surf_image *img, int16_t frame_w, int16_t frame_h,
                              int16_t x, int16_t y)
{
    if (!img || frame_w <= 0 || frame_h <= 0 || img->w < frame_w || img->h < frame_h)
        return NULL;
    surf_node *n = node_alloc(SURF_NODE_FILMSTRIP);
    if (n) {
        n->x = x; n->y = y; n->w = frame_w; n->h = frame_h;
        n->u.strip.img = img;
        n->u.strip.fw = frame_w;
        n->u.strip.fh = frame_h;
        n->u.strip.per_row = (int16_t)(img->w / frame_w);
        n->u.strip.nframes = (int16_t)(n->u.strip.per_row * (img->h / frame_h));
    }
    return n;
}

surf_node *surf_ninepatch_new(const surf_image *img, int16_t x, int16_t y,
                              int16_t w, int16_t h,
                              int16_t l, int16_t t, int16_t r, int16_t b)
{
    if (!img || l < 0 || t < 0 || r < 0 || b < 0 || l + r > img->w || t + b > img->h)
        return NULL;
    surf_node *n = node_alloc(SURF_NODE_NINEPATCH);
    if (n) {
        n->x = x; n->y = y; n->w = w; n->h = h;
        n->u.nine.img = img;
        n->u.nine.l = l; n->u.nine.t = t; n->u.nine.r = r; n->u.nine.b = b;
    }
    return n;
}

/* ---- tree ---- */

void surf_node_add(surf_node *parent, surf_node *child)
{
    if (!parent || !child || child->parent ||
        (parent->type != SURF_NODE_GROUP && parent->type != SURF_NODE_SCROLLVIEW))
        return;
    child->prev = parent->last;
    child->next = NULL;
    if (parent->last)
        parent->last->next = child;
    else
        parent->first = child;
    parent->last = child;
    child->parent = parent;
    surf_damage_subtree(child);
}

void surf_node_detach(surf_node *child)
{
    if (!child || !child->parent)
        return;
    for (surf_node *c = surf_g.capture; c; c = c->parent)
        if (c == child) { surf_g.capture = NULL; break; }
    surf_damage_subtree(child);
    surf_node *p = child->parent;
    if (child->prev) child->prev->next = child->next; else p->first = child->next;
    if (child->next) child->next->prev = child->prev; else p->last = child->prev;
    child->parent = NULL;
    child->prev = child->next = NULL;
}

static void destroy_children(surf_node *n)
{
    surf_node *c = n->first;
    while (c) {
        surf_node *next = c->next;
        destroy_children(c);
        surf_text_free_storage(c);
        node_free(c);
        c = next;
    }
    n->first = n->last = NULL;
}

void surf_node_destroy(surf_node *n)
{
    if (!n || n == surf_g.root)
        return;
    surf_node_detach(n);
    destroy_children(n);
    surf_text_free_storage(n);
    node_free(n);
}

/* ---- properties: damage old rect, mutate, damage new rect ---- */

void surf_node_set_pos(surf_node *n, int16_t x, int16_t y)
{
    if (!n || (n->x == x && n->y == y))
        return;
    /* A small move damages ONE union rect, not old + new: adjacent
     * rects never coalesce (dirty merge needs overlap), so per-frame
     * movers used to cost two entries each — six bullets and a ship
     * overflowed the 16-entry list and degraded to a full-screen
     * union (measured: 50 -> 11 fps). Union only when it wastes
     * little area; a teleport still damages two separate rects. */
    surf_rect a = subtree_screen_rect(n);
    n->x = x;
    n->y = y;
    surf_rect b = subtree_screen_rect(n);
    surf_rect u = surf_rect_union(a, b);
    int32_t ua = (int32_t)u.w * u.h;
    int32_t sa = (int32_t)a.w * a.h + (int32_t)b.w * b.h;
    if (ua <= sa + sa / 4) {
        if (!surf_rect_empty(u))
            surf_dirty_add(&surf_g.dirty, u);
    } else {
        if (!surf_rect_empty(a))
            surf_dirty_add(&surf_g.dirty, a);
        if (!surf_rect_empty(b))
            surf_dirty_add(&surf_g.dirty, b);
    }
}

void surf_node_set_hidden(surf_node *n, bool hidden)
{
    if (!n || hidden == !!(n->flags & SURF_NF_HIDDEN))
        return;
    surf_damage_subtree(n);
    if (hidden) n->flags |= SURF_NF_HIDDEN; else n->flags &= (uint8_t)~SURF_NF_HIDDEN;
    surf_damage_subtree(n);
}

void surf_rect_set_color(surf_node *n, surf_color c)
{
    if (!n || n->type != SURF_NODE_RECT || n->u.rect.color == c)
        return;
    n->u.rect.color = c;
    surf_damage_subtree(n);
}

void surf_rect_set_size(surf_node *n, int16_t w, int16_t h)
{
    if (!n || n->type != SURF_NODE_RECT || (n->w == w && n->h == h))
        return;
    surf_damage_subtree(n);
    n->w = w;
    n->h = h;
    surf_damage_subtree(n);
}

void surf_node_damage(surf_node *n)
{
    if (n)
        surf_damage_subtree(n);
}

void surf_sprite_set_fast_pan(surf_node *n, bool on)
{
    if (n && n->type == SURF_NODE_SPRITE)
        n->u.sprite.fast_pan = on;
}

void surf_sprite_set_src(surf_node *n, surf_rect src)
{
    if (!n || n->type != SURF_NODE_SPRITE)
        return;
    surf_rect old = n->u.sprite.src;
    int32_t dx = src.x - old.x, dy = src.y - old.y;
    bool pan_only = src.w == old.w && src.h == old.h;

    if (pan_only && dx == 0 && dy == 0) {
        /* No-op call while streaming: refresh the band with a ZERO
         * shift, not a repaint — a camera crawling at sub-pixel speed
         * calls this between every 1px step, and a repaint per step is
         * what made parallax fps track the scroll speed (45-69 fps by
         * ship x, measured). The zero shift keeps the stream alive at
         * the cost of one band copy; the heal (full repaint) only runs
         * when streaming can't continue. */
        if (n->u.sprite.pan_shifted) {
            bool alive = n->u.sprite.fast_pan && surf_g.hal->band_shift &&
                         surf_node_attached(n) && !(n->flags & SURF_NF_HIDDEN);
            if (alive) {
                int16_t zx, zy;
                surf_node_abs_pos(n, &zx, &zy);
                surf_g.hal->band_shift((surf_rect){zx, zy, n->w, n->h}, 0, 0);
            } else {
                n->u.sprite.pan_shifted = false;
                surf_damage_subtree(n);
            }
        }
        return;
    }

    int16_t ax, ay;
    bool can_fast = pan_only && n->u.sprite.fast_pan && surf_g.hal->band_shift &&
                    n->u.sprite.img->opaque &&
                    n->u.sprite.scale_q16 == SURF_ONE && n->u.sprite.rot == 0 &&
                    n->u.sprite.mirror == 0 && surf_node_attached(n) &&
                    !(n->flags & SURF_NF_HIDDEN) &&
                    dx > -src.w && dx < src.w && dy > -src.h && dy < src.h;
    if (can_fast) {
        surf_node_abs_pos(n, &ax, &ay);
        surf_rect band = {ax, ay, n->w, n->h};
        surf_rect on_scr = surf_rect_intersect(
            band, (surf_rect){0, 0, surf_g.w, surf_g.h});
        can_fast = on_scr.w == band.w && on_scr.h == band.h;
        for (const surf_node *p = n->parent; can_fast && p; p = p->parent)
            if (p->type == SURF_NODE_SCROLLVIEW || (p->flags & SURF_NF_CLIP))
                can_fast = false;
        if (can_fast) {
            n->u.sprite.src = src;
            surf_g.hal->band_shift(band, (int16_t)-dx, (int16_t)-dy);
            n->u.sprite.pan_shifted = true;
            int16_t adx = (int16_t)(dx < 0 ? -dx : dx);
            int16_t ady = (int16_t)(dy < 0 ? -dy : dy);
            /* disjoint L: the vertical sliver owns the corner — touching
             * rects don't coalesce, overlapping ones would merge the L
             * into a full-band repaint */
            if (adx)
                surf_dirty_add(&surf_g.dirty, (surf_rect){
                    dx > 0 ? (int16_t)(band.x + band.w - adx) : band.x,
                    band.y, adx, band.h});
            if (ady)
                surf_dirty_add(&surf_g.dirty, (surf_rect){
                    dx > 0 ? band.x : (int16_t)(band.x + adx),
                    dy > 0 ? (int16_t)(band.y + band.h - ady) : band.y,
                    (int16_t)(band.w - adx), ady});
            /* overlays (later siblings) smeared by the shift */
            for (surf_node *s = n->next; s; s = s->next) {
                if (s->flags & SURF_NF_HIDDEN)
                    continue;
                int16_t sx, sy;
                surf_node_abs_pos(s, &sx, &sy);
                surf_rect r = {(int16_t)(sx - adx), (int16_t)(sy - ady),
                               (int16_t)(s->w + 2 * adx),
                               (int16_t)(s->h + 2 * ady)};
                r = surf_rect_intersect(r, band);
                if (!surf_rect_empty(r))
                    surf_dirty_add(&surf_g.dirty, r);
            }
            return;
        }
    }

    n->u.sprite.pan_shifted = false;
    surf_damage_subtree(n);
    n->u.sprite.src = src;
    sprite_update_size(n);
    surf_damage_subtree(n);
}

void surf_sprite_set_xform(surf_node *n, int32_t scale_q16, uint8_t rot,
                           uint8_t mirror)
{
    if (!n || n->type != SURF_NODE_SPRITE || scale_q16 <= 0)
        return;
    /* the PPA SRM range; keep every backend honest about it */
    if (scale_q16 < SURF_ONE / 16) scale_q16 = SURF_ONE / 16;
    if (scale_q16 > SURF_ONE * 16) scale_q16 = SURF_ONE * 16;
    rot &= 3;
    mirror &= 3;
    if (scale_q16 == n->u.sprite.scale_q16 && rot == n->u.sprite.rot &&
        mirror == n->u.sprite.mirror)
        return;
    surf_damage_subtree(n);
    n->u.sprite.scale_q16 = scale_q16;
    n->u.sprite.rot = rot;
    n->u.sprite.mirror = mirror;
    sprite_update_size(n);
    surf_damage_subtree(n);
}

int32_t surf_sprite_scale(const surf_node *n)
{
    return (n && n->type == SURF_NODE_SPRITE) ? n->u.sprite.scale_q16 : SURF_ONE;
}

uint8_t surf_sprite_rot(const surf_node *n)
{
    return (n && n->type == SURF_NODE_SPRITE) ? n->u.sprite.rot : 0;
}

uint8_t surf_sprite_mirror(const surf_node *n)
{
    return (n && n->type == SURF_NODE_SPRITE) ? n->u.sprite.mirror : 0;
}

surf_node *surf_layer_new(const surf_image *strip, int16_t x, int16_t y,
                          int16_t view_w)
{
    if (!strip || view_w <= 0)
        return NULL;
    surf_node *n = node_alloc(SURF_NODE_LAYER);
    if (n) {
        n->x = x; n->y = y; n->w = view_w; n->h = strip->h;
        n->u.layer.strip = strip;
        n->u.layer.off_q16 = 0;
        n->u.layer.fast = false;
        n->u.layer.shifted = false;
    }
    return n;
}

int32_t surf_layer_offset(const surf_node *n)
{
    return (n && n->type == SURF_NODE_LAYER) ? n->u.layer.off_q16 : 0;
}

void surf_layer_set_fast_scroll(surf_node *n, bool on)
{
    if (n && n->type == SURF_NODE_LAYER)
        n->u.layer.fast = on;
}

void surf_layer_set_offset(surf_node *n, int32_t off_q16)
{
    if (!n || n->type != SURF_NODE_LAYER)
        return;
    int32_t wrap = (int32_t)n->u.layer.strip->w << 16;
    off_q16 %= wrap;
    if (off_q16 < 0)
        off_q16 += wrap;
    int32_t old_px = n->u.layer.off_q16 >> 16;
    n->u.layer.off_q16 = off_q16;
    int32_t dx = (off_q16 >> 16) - old_px;
    if (dx == 0) {
        /* Sub-pixel frame while streaming: zero shift, not a repaint
         * (same rule and same measured reason as sprite fast pan). */
        if (n->u.layer.shifted) {
            bool alive = n->u.layer.fast && surf_g.hal->band_shift &&
                         surf_node_attached(n) && !(n->flags & SURF_NF_HIDDEN);
            if (alive) {
                int16_t zx, zy;
                surf_node_abs_pos(n, &zx, &zy);
                surf_g.hal->band_shift((surf_rect){zx, zy, n->w, n->h}, 0, 0);
            } else {
                n->u.layer.shifted = false;
                surf_damage_subtree(n);
            }
        }
        return;
    }
    /* wrap distance: shift the short way around */
    int32_t sw = n->u.layer.strip->w;
    if (dx > sw / 2) dx -= sw;
    if (dx < -sw / 2) dx += sw;

    int16_t ax, ay;
    surf_node_abs_pos(n, &ax, &ay);
    surf_rect band = {ax, ay, n->w, n->h};
    surf_rect on = surf_rect_intersect(band, (surf_rect){0, 0, surf_g.w, surf_g.h});
    bool can_fast = n->u.layer.fast && surf_g.hal->band_shift &&
                    n->u.layer.strip->opaque && surf_node_attached(n) &&
                    !(n->flags & SURF_NF_HIDDEN) &&
                    on.w == band.w && on.h == band.h &&
                    dx > -band.w && dx < band.w;
    for (const surf_node *p = n->parent; can_fast && p; p = p->parent)
        if (p->type == SURF_NODE_SCROLLVIEW || (p->flags & SURF_NF_CLIP))
            can_fast = false;
    if (!can_fast) {
        n->u.layer.shifted = false;
        surf_damage_subtree(n);
        return;
    }

    /* content moves opposite the offset */
    surf_g.hal->band_shift(band, (int16_t)-dx, 0);
    n->u.layer.shifted = true;
    int16_t adx = (int16_t)(dx < 0 ? -dx : dx);
    surf_rect sliver = dx > 0
        ? (surf_rect){(int16_t)(band.x + band.w - adx), band.y, adx, band.h}
        : (surf_rect){band.x, band.y, adx, band.h};
    surf_dirty_add(&surf_g.dirty, sliver);

    /* anything drawn over the band (later siblings) just got smeared by
     * the shift: repaint it, expanded by the shift so the ghost goes too */
    for (surf_node *s = n->next; s; s = s->next) {
        if (s->flags & SURF_NF_HIDDEN)
            continue;
        int16_t sx, sy;
        surf_node_abs_pos(s, &sx, &sy);
        surf_rect r = {(int16_t)(sx - adx), sy,
                       (int16_t)(s->w + 2 * adx), s->h};
        r = surf_rect_intersect(r, band);
        if (!surf_rect_empty(r))
            surf_dirty_add(&surf_g.dirty, r);
    }
}

void surf_group_set_clip(surf_node *g, int16_t w, int16_t h)
{
    if (!g || g->type != SURF_NODE_GROUP)
        return;
    surf_damage_subtree(g);
    g->w = w;
    g->h = h;
    if (w > 0 && h > 0) g->flags |= SURF_NF_CLIP; else g->flags &= (uint8_t)~SURF_NF_CLIP;
    surf_damage_subtree(g);
}

void surf_filmstrip_set_frame(surf_node *n, int16_t frame)
{
    if (!n || n->type != SURF_NODE_FILMSTRIP)
        return;
    if (frame < 0) frame = 0;
    if (frame >= n->u.strip.nframes) frame = (int16_t)(n->u.strip.nframes - 1);
    if (frame == n->u.strip.frame)
        return;
    n->u.strip.frame = frame;
    surf_damage_subtree(n);  /* bounds unchanged: one rect covers old + new */
}

int16_t surf_filmstrip_frame(const surf_node *n)
{
    return (n && n->type == SURF_NODE_FILMSTRIP) ? n->u.strip.frame : 0;
}

void surf_ninepatch_set_size(surf_node *n, int16_t w, int16_t h)
{
    if (!n || n->type != SURF_NODE_NINEPATCH || (n->w == w && n->h == h))
        return;
    surf_damage_subtree(n);
    n->w = w;
    n->h = h;
    surf_damage_subtree(n);
}

void surf_node_set_on_touch(surf_node *n, surf_touch_cb cb, void *user)
{
    if (!n)
        return;
    n->on_touch = cb;
    n->touch_user = user;
}

void surf_node_abs_pos(const surf_node *n, int16_t *x, int16_t *y)
{
    int16_t ax = 0, ay = 0;
    for (; n; n = n->parent) {
        ax = (int16_t)(ax + n->x);
        ay = (int16_t)(ay + n->y);
        if (n->parent && n->parent->type == SURF_NODE_SCROLLVIEW) {
            ax = (int16_t)(ax - (n->parent->u.scroll.off_x >> 16));
            ay = (int16_t)(ay - (n->parent->u.scroll.off_y >> 16));
        }
    }
    if (x) *x = ax;
    if (y) *y = ay;
}

surf_point surf_node_pos(const surf_node *n)
{
    return n ? (surf_point){n->x, n->y} : (surf_point){0, 0};
}

surf_point surf_node_size(const surf_node *n)
{
    return n ? (surf_point){n->w, n->h} : (surf_point){0, 0};
}

void surf_inject_touch(const surf_touch *t)
{
    if (surf_g.root && t)
        surf_input_dispatch(t);
}

void surf_node_set_gesture_grab(surf_node *n, bool grab)
{
    if (!n)
        return;
    if (grab)
        n->flags |= SURF_NF_GRAB;
    else
        n->flags &= (uint8_t)~SURF_NF_GRAB;
}
