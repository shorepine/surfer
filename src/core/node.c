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
    n->opa = 255;
    return n;
}
#define node_alloc surf_node_alloc

/* Set by a binding that keeps one wrapper object per node. Static rather
 * than part of surf_g, so it survives deinit/re-init: the binding
 * installs it once and a soft reset must not silently unhook it. */
static surf_node_freed_fn g_freed_cb;

void surf_set_node_freed_cb(surf_node_freed_fn cb)
{
    g_freed_cb = cb;
}

int surf_node_index(const surf_node *n)
{
    if (!n || !surf_g.pool)
        return -1;
    ptrdiff_t i = n - surf_g.pool;
    return (i >= 0 && i < surf_g.pool_cap) ? (int)i : -1;
}

static void node_free(surf_node *n)
{
    /* Only real destroys. surf_init threads the whole pool onto the free
     * list through here, and those slots were never anybody's node --
     * calloc left them SURF_NODE_FREE, which is what tells them apart. */
    if (g_freed_cb && n->type != SURF_NODE_FREE)
        g_freed_cb(n, surf_node_index(n));
    /* every contact, not just one: a destroyed node may be holding any
     * of the fingers currently down */
    for (int i = 0; i < SURF_MAX_CONTACTS; i++) {
        if (surf_g.contacts[i].capture == n)
            surf_g.contacts[i].capture = NULL;
        if (surf_g.contacts[i].steal_sv == n)
            surf_g.contacts[i].steal_sv = NULL;
    }
    if (n->type == SURF_NODE_SCROLLVIEW)
        surf_scroll_forget(n);
    if (n->type == SURF_NODE_SPRITE && n->u.sprite.hb) {
        free(n->u.sprite.hb);
        n->u.sprite.hb = NULL; n->u.sprite.hbn = 0;
    }
    if (n->type == SURF_NODE_FILMSTRIP && n->u.strip.hb) {
        free(n->u.strip.hb);
        n->u.strip.hb = NULL; n->u.strip.hbn = 0;
    }
    /* A PLAYING strip that is destroyed must give its count back, or the
     * scan in surf_filmstrip_tick keeps running for an animation nobody
     * owns — and after enough app quits it never stops running. */
    if (n->type == SURF_NODE_FILMSTRIP && n->u.strip.fps_q16
        && !n->u.strip.paused)
        surf_g.playing--;
    /* A tween outliving its node is a write through a freed pointer every
     * frame. Gated on the counter, so surf_init's 4096 node_free calls
     * cost one comparison each rather than a table scan. */
    if (surf_g.ntweens > 0)
        surf_tween_drop(n, -1);
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
    /* the screen's SIZE, with no clip flag: clipping and hit testing key
     * on SURF_NF_CLIP and a handler, so this changes neither -- it only
     * lets surf_node_size(surf_screen()) answer, which a widget placing
     * a popup (the dropdown) needs and cannot get any other way through
     * the public API */
    if (surf_g.root) {
        surf_g.root->w = w;
        surf_g.root->h = h;
    }
    surf_pad_reset_all();
    surf_key_reset();
    surf_wheel_reset();
    surf_dirty_reset(&surf_g.dirty, (surf_rect){0, 0, w, h});
    surf_dirty_add(&surf_g.dirty, (surf_rect){0, 0, w, h});
    return true;
}

void surf_deinit(void)
{
    /* the ink table keys on image POINTERS, and a soft reset frees
     * images whose addresses malloc will hand out again — a stale entry
     * would then answer for a picture it has never seen */
    surf_ink_reset();
    surf_mesh_reset();
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
    surf_filmstrip_tick();  /* playing animations advance themselves */
    surf_tween_tick();      /* ...and so do property tweens */
    surf_compose();
    if (surf_g.frame_div > 0 && surf_g.hal->wait_frame)
        surf_g.hal->wait_frame(surf_g.frame_div);
}

int surf_touch_points(surf_touch_pt *out, int max)
{
    if (!surf_g.hal || !surf_g.hal->touch_points || max <= 0)
        return 0;
    return surf_g.hal->touch_points(out, max);
}

void surf_set_frame_divisor(int divisor)
{
    surf_g.frame_div = divisor > 0 ? divisor : 0;
}

float surf_frame_hz(void)
{
    if (surf_g.hal && surf_g.hal->frame_hz)
        return surf_g.hal->frame_hz();
    return 60.0f;
}

/* ---- damage ---- */

bool surf_node_attached(const surf_node *n)
{
    while (n->parent)
        n = n->parent;
    return n == surf_g.root;
}

/* Hidden by its OWN flag or by any ancestor's — a node inside a hidden
 * group paints nothing, so it owns none of the pixels under it.
 *
 * This is what every hal-shift gate has to ask, not `flags & HIDDEN`.
 * The four shift paths hand a screen rect to the hal to move in place,
 * and the hal moves real framebuffer pixels: ask the wrong question and
 * a layer scrolling inside a backgrounded app's hidden group drags
 * whatever app IS on screen sideways at its own scroll rate. The node's
 * own flag was never the whole test, only the common half of it. */
bool surf_node_effectively_hidden(const surf_node *n)
{
    for (; n; n = n->parent)
        if (n->flags & SURF_NF_HIDDEN)
            return true;
    return false;
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
        n->u.sprite.xf.scale_q16 = SURF_ONE;
        n->u.sprite.xf.rot = 0;
        n->u.sprite.xf.mirror = 0;
    }
    return n;
}

/* The transform of a node that HAS one, or NULL. A sprite and a
 * filmstrip both do; nothing else does. */
surf_xform *surf_node_xform(surf_node *n)
{
    if (!n)
        return NULL;
    if (n->type == SURF_NODE_SPRITE)
        return &n->u.sprite.xf;
    if (n->type == SURF_NODE_FILMSTRIP)
        return &n->u.strip.xf;
    return NULL;
}

/* What the transform is applied TO: a sprite's source rect, or one
 * FRAME of a strip. The only line where the two differ. */
static void xform_source(const surf_node *n, int32_t *w, int32_t *h)
{
    if (n->type == SURF_NODE_FILMSTRIP) {
        *w = n->u.strip.fw;
        *h = n->u.strip.fh;
    } else {
        *w = n->u.sprite.src.w;
        *h = n->u.sprite.src.h;
    }
}

/* node w/h = the on-screen footprint: source scaled, sides swapped for
 * quarter-turn rotations */
static void sprite_update_size(surf_node *n)
{
    const surf_xform *xf = surf_node_xform(n);
    int32_t sw, sh;
    if (!xf)
        return;
    xform_source(n, &sw, &sh);
    int32_t w = (int32_t)(((int64_t)sw * xf->scale_q16) >> 16);
    int32_t h = (int32_t)(((int64_t)sh * xf->scale_q16) >> 16);
    if (w < 1) w = 1;
    if (h < 1) h = 1;
    if (xf->rot & 1) {
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
        /* 1:1 EXPLICITLY. node_alloc zeroes the union and a scale of 0
         * is not "unscaled", it is a footprint of nothing — the same
         * reason surf_sprite_new sets it rather than leaning on the
         * zeroing. */
        n->u.strip.xf.scale_q16 = SURF_ONE;
        n->u.strip.xf.rot = 0;
        n->u.strip.xf.mirror = 0;
    }
    return n;
}

/* Is the 9-patch's centre band a single opaque colour? Scanned ONCE,
 * here, because a stretched centre is tiled one source-tile at a time and
 * each tile is a hal op — a 356px scrollbar thumb off a 4px band is ~89
 * ops at the PPA's ~85us floor, twice a frame while it moves. A capsule's
 * ROUND CAPS carry alpha but its centre does not, so this asks about the
 * centre rather than trusting img->opaque.
 *
 * Not a frame-path pixel loop (DESIGN.md's first rule): it runs at
 * construction, over the source's centre, which is tens of pixels. */
static bool region_is_solid(const surf_image *img, int x0, int y0,
                            int w, int h, surf_color *out)
{
    const int l = x0, t = y0;
    if (w <= 0 || h <= 0 || !img->pixels)
        return false;
    if (img->format == SURF_FMT_RGB565) {
        const uint8_t *base = (const uint8_t *)img->pixels;
        surf_color first = *(const surf_color *)(base + (size_t)t * img->stride
                                                 + (size_t)l * 2);
        for (int yy = 0; yy < h; yy++) {
            const surf_color *row = (const surf_color *)
                (base + (size_t)(t + yy) * img->stride + (size_t)l * 2);
            for (int xx = 0; xx < w; xx++)
                if (row[xx] != first)
                    return false;
        }
        *out = first;
        return true;
    }
    if (img->format == SURF_FMT_ARGB8888) {
        const uint8_t *base = (const uint8_t *)img->pixels;
        uint32_t first = *(const uint32_t *)(base + (size_t)t * img->stride
                                             + (size_t)l * 4);
        if ((first >> 24) != 0xff)          /* translucent: must blend */
            return false;
        for (int yy = 0; yy < h; yy++) {
            const uint32_t *row = (const uint32_t *)
                (base + (size_t)(t + yy) * img->stride + (size_t)l * 4);
            for (int xx = 0; xx < w; xx++)
                if (row[xx] != first)
                    return false;
        }
        *out = SURF_RGB((first >> 16) & 0xff, (first >> 8) & 0xff, first & 0xff);
        return true;
    }
    return false;                            /* A8 is tinted; leave it alone */
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
        int sx[4] = {0, l, img->w - r, img->w};
        int sy[4] = {0, t, img->h - b, img->h};
        for (int ry = 0; ry < 3; ry++)
            for (int rx = 0; rx < 3; rx++)
                n->u.nine.solid[ry][rx] = region_is_solid(
                    img, sx[rx], sy[ry], sx[rx + 1] - sx[rx],
                    sy[ry + 1] - sy[ry], &n->u.nine.solid_col[ry][rx]);
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
    /* Detaching the captured node releases the capture — picking a card
     * up raises it, raising is detach + re-add, and doing that in a DOWN
     * handler killed the drag on its first move. Per contact now, and
     * per contact it is the same rule. */
    for (int i = 0; i < SURF_MAX_CONTACTS; i++)
        for (surf_node *c = surf_g.contacts[i].capture; c; c = c->parent)
            if (c == child) { surf_g.contacts[i].capture = NULL; break; }
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

/* The move itself, cancelling nothing — what a running X or Y tween
 * calls. The public setter below kills those tweens first. */
static void node_set_pos_raw(surf_node *n, int16_t x, int16_t y)
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

void surf_node_set_pos(surf_node *n, int16_t x, int16_t y)
{
    /* A DIRECT MOVE WINS over a tween walking the same axis, or the
     * tween overwrites it on the next tick and the write silently does
     * nothing. Both axes, because set_pos writes both. */
    surf_tween_drop(n, SURF_TW_X);
    surf_tween_drop(n, SURF_TW_Y);
    node_set_pos_raw(n, x, y);
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

/* Which nodes carry an opacity: the ones painted through hal->blend.
 * See surf_node_set_opacity's note in surfer.h for why a rect and a
 * textgrid (hal->fill) and a group (no render target) are not on it. */
bool surf_node_can_fade(const surf_node *n)
{
    if (!n)
        return false;
    switch (n->type) {
    case SURF_NODE_SPRITE:
    case SURF_NODE_FILMSTRIP:
    case SURF_NODE_NINEPATCH:
    case SURF_NODE_LAYER:
    case SURF_NODE_TEXT:
        return true;
    default:
        return false;
    }
}

/* The write itself, without touching any tween — what surf_fade_tick
 * calls sixty times a second. */
static void fade_apply(surf_node *n, uint8_t opa)
{
    if (n->opa == opa)
        return;
    /* Crossing 255 in either direction changes whether this node COVERS
     * what is under it, and the compositor's occlusion early-out is
     * decided per dirty rect at paint time — so the damage has to be the
     * subtree either way and the layers below get repainted with it. */
    n->opa = opa;
    surf_damage_subtree(n);
}

bool surf_node_set_opacity(surf_node *n, uint8_t opa)
{
    if (!surf_node_can_fade(n))
        return false;
    /* A DIRECT WRITE WINS. Without this a running fade would overwrite it
     * on the very next tick, so `spr.opacity = 1` in the middle of a
     * fade-out would appear to do nothing at all — the shape of silent
     * failure this API spent a whole section avoiding. */
    surf_tween_drop(n, SURF_TW_OPACITY);
    fade_apply(n, opa);
    return true;
}

/* ---- tweens: a node property over time, driven by surf_tick ---- */

static void node_set_pos_raw(surf_node *n, int16_t x, int16_t y);
static void xform_apply(surf_node *n, int32_t scale_q16, uint8_t rot,
                        uint8_t mirror);
static void fade_apply(surf_node *n, uint8_t opa);

/* The write, per property, WITHOUT cancelling anything — this is what
 * the tick calls sixty times a second, and it must not kill the tween
 * that is driving it. The public setters cancel and then land here. */
static void tw_write(surf_node *n, uint8_t prop, int32_t q16)
{
    switch (prop) {
    case SURF_TW_OPACITY: {
        int32_t v = q16 >> 16;
        if (v < 0) v = 0;
        if (v > 255) v = 255;
        fade_apply(n, (uint8_t)v);
        return;
    }
    case SURF_TW_X:
        node_set_pos_raw(n, (int16_t)(q16 >> 16), n->y);
        return;
    case SURF_TW_Y:
        node_set_pos_raw(n, n->x, (int16_t)(q16 >> 16));
        return;
    case SURF_TW_SCALE: {
        surf_xform *xf = surf_node_xform(n);
        if (xf)
            xform_apply(n, q16, xf->rot, xf->mirror);
        return;
    }
    default:
        return;
    }
}

/* Where a property is NOW, in the same Q16 the tween interpolates. */
int32_t surf_node_tween_value(const surf_node *n, uint8_t prop)
{
    if (!n)
        return 0;
    switch (prop) {
    case SURF_TW_OPACITY: return (int32_t)n->opa << 16;
    case SURF_TW_X:       return (int32_t)n->x << 16;
    case SURF_TW_Y:       return (int32_t)n->y << 16;
    case SURF_TW_SCALE:   return surf_sprite_scale(n);
    default:              return 0;
    }
}

/* Which properties a node type actually has. Refused rather than
 * quietly ignored — the .rot-on-a-group lesson. */
static bool tw_can(const surf_node *n, uint8_t prop)
{
    if (!n || prop >= SURF_TW_NPROPS)
        return false;
    if (prop == SURF_TW_OPACITY)
        return surf_node_can_fade(n);
    if (prop == SURF_TW_SCALE)
        return surf_node_can_xform(n);
    return true;                      /* every node has a position */
}

static surf_tween *tw_of(const surf_node *n, int prop)
{
    if (surf_g.ntweens <= 0)
        return NULL;
    for (int i = 0; i < SURF_MAX_TWEENS; i++) {
        surf_tween *t = &surf_g.tweens[i];
        if (t->n == n && (prop < 0 || t->prop == (uint8_t)prop))
            return t;
    }
    return NULL;
}

void surf_tween_drop(surf_node *n, int prop)
{
    if (surf_g.ntweens <= 0)
        return;
    for (int i = 0; i < SURF_MAX_TWEENS; i++) {
        surf_tween *t = &surf_g.tweens[i];
        if (t->n == n && (prop < 0 || t->prop == (uint8_t)prop)) {
            t->n = NULL;
            surf_g.ntweens--;
            if (prop >= 0)
                return;               /* one property: there is only one */
        }
    }
}

void surf_node_tween_cancel(surf_node *n, int prop)
{
    surf_tween_drop(n, prop);
}

bool surf_node_tweening(const surf_node *n, int prop)
{
    return tw_of(n, prop) != NULL;
}

bool surf_node_tween(surf_node *n, uint8_t prop, int32_t to_q16,
                     int32_t ms, uint8_t ease)
{
    if (!tw_can(n, prop))
        return false;
    surf_tween_drop(n, prop);         /* a new one replaces the old */
    int32_t from = surf_node_tween_value(n, prop);
    /* Nothing to animate: no duration, no distance, or no clock to
     * measure with. Land on the value rather than refusing — a caller
     * asking to end up at `to` should end up at `to`. */
    if (ms <= 0 || from == to_q16 || !surf_g.hal || !surf_g.hal->now_us) {
        tw_write(n, prop, to_q16);
        return true;
    }
    for (int i = 0; i < SURF_MAX_TWEENS; i++) {
        surf_tween *t = &surf_g.tweens[i];
        if (t->n)
            continue;
        t->n = n;
        t->prop = prop;
        t->ease = ease > SURF_EASE_IN_OUT ? SURF_EASE_LINEAR : ease;
        t->from = from;
        t->to = to_q16;
        t->t0_us = surf_g.hal->now_us();
        t->dur_us = (uint32_t)ms * 1000u;
        surf_g.ntweens++;
        return true;
    }
    tw_write(n, prop, to_q16);        /* table full: instant, not ignored */
    return true;
}

/* p is Q16 progress 0..1. Quadratic, in fixed point — DESIGN.md's habit,
 * and the products fit int64 with room to spare. */
static int32_t ease_at(uint8_t how, int32_t p)
{
    int64_t q = p;
    switch (how) {
    case SURF_EASE_IN:
        return (int32_t)((q * q) >> 16);
    case SURF_EASE_OUT: {
        int64_t inv = SURF_ONE - q;
        return (int32_t)(SURF_ONE - ((inv * inv) >> 16));
    }
    case SURF_EASE_IN_OUT:
        if (q < SURF_ONE / 2)
            return (int32_t)((2 * q * q) >> 16);
        else {
            int64_t inv = SURF_ONE - q;
            return (int32_t)(SURF_ONE - ((2 * inv * inv) >> 16));
        }
    default:
        return p;
    }
}

void surf_tween_tick(void)
{
    if (surf_g.ntweens <= 0 || !surf_g.hal || !surf_g.hal->now_us)
        return;
    uint64_t now = surf_g.hal->now_us();
    for (int i = 0; i < SURF_MAX_TWEENS; i++) {
        surf_tween *t = &surf_g.tweens[i];
        if (!t->n)
            continue;
        uint64_t el = now - t->t0_us;
        if (el >= t->dur_us) {
            surf_node *n = t->n;
            uint8_t prop = t->prop;
            int32_t to = t->to;
            t->n = NULL;               /* clear BEFORE applying: the write
                                        * damages, and a half-freed slot
                                        * seen from a damage path would be
                                        * a tween that never ends */
            surf_g.ntweens--;
            tw_write(n, prop, to);
            continue;
        }
        int32_t p = (int32_t)(((int64_t)el << 16) / (int64_t)t->dur_us);
        p = ease_at(t->ease, p);
        int64_t span = (int64_t)t->to - (int64_t)t->from;
        tw_write(t->n, t->prop, t->from + (int32_t)((span * p) >> 16));
    }
}

/* ---- opacity's named shortcuts ---- */

bool surf_node_fade_to(surf_node *n, uint8_t to, int32_t ms)
{
    return surf_node_tween(n, SURF_TW_OPACITY, (int32_t)to << 16, ms,
                           SURF_EASE_LINEAR);
}

void surf_node_fade_cancel(surf_node *n)
{
    surf_tween_drop(n, SURF_TW_OPACITY);
}

bool surf_node_fading(const surf_node *n)
{
    return tw_of(n, SURF_TW_OPACITY) != NULL;
}

uint8_t surf_node_opacity(const surf_node *n)
{
    return n ? n->opa : 255;
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

/* ---- overlaps: boxes first, then INK ----------------------------------
 * hits() used to be the two bounding boxes alone, and the bug report
 * that ended that was exact: a ball "bouncing off a sword way before
 * contact" — a longsword is a diagonal of ink inside a mostly
 * transparent square, so the box collided at the empty corner. A sprite
 * or filmstrip now answers from its image's alpha (surf_ink, image.c);
 * everything else — rects, groups, labels — is still its box, and so is
 * any image with nothing transparent in it.
 *
 * The per-pixel walk is legal by the colorpicker's rule: it runs on an
 * EVENT (an app asking, from its own frame) and never in the compose
 * path, it is bounded by the box INTERSECTION — which at the moment of
 * contact is small — and the mask it reads is 1 bit per pixel, built
 * once and cached until the image's pixels change. */

typedef struct {
    const uint32_t *bits;   /* whole-image mask, LSB-first words */
    int32_t wpr;            /* words per row */
    surf_rect src;          /* the cell this node draws (sprite src / strip frame) */
    const surf_xform *xf;
    int16_t iw, ih;         /* image bounds, for the clamp */
} ink_view;

static bool node_ink(const surf_node *n, ink_view *v)
{
    const surf_image *img;
    if (n->type == SURF_NODE_SPRITE && n->u.sprite.img) {
        img = n->u.sprite.img;
        v->src = n->u.sprite.src;
        v->xf = &n->u.sprite.xf;
    } else if (n->type == SURF_NODE_FILMSTRIP && n->u.strip.img &&
               n->u.strip.per_row > 0) {
        img = n->u.strip.img;
        v->src = (surf_rect){
            (int16_t)((n->u.strip.frame % n->u.strip.per_row) * n->u.strip.fw),
            (int16_t)((n->u.strip.frame / n->u.strip.per_row) * n->u.strip.fh),
            n->u.strip.fw, n->u.strip.fh};
        v->xf = &n->u.strip.xf;
    } else {
        return false;
    }
    v->bits = surf_ink(img, &v->wpr);
    if (!v->bits)
        return false;               /* opaque, 565, or OOM: the box is right */
    v->iw = img->w;
    v->ih = img->h;
    return v->src.w > 0 && v->src.h > 0;
}

/* Is there ink at (dx, dy) of the node's on-screen footprint? The
 * inverse map is the SDL hal's h_xform_blend arithmetic exactly — rot is
 * quarter turns CCW, mirror flips the source before rotation — so what
 * collides is what is drawn, not an approximation of it. */
static bool ink_at(const ink_view *v, const surf_node *n, int32_t dx, int32_t dy)
{
    int32_t W0 = (v->xf->rot & 1) ? n->h : n->w;   /* pre-rotation footprint */
    int32_t H0 = (v->xf->rot & 1) ? n->w : n->h;
    if (W0 <= 0 || H0 <= 0)
        return false;
    int32_t ux, uy;
    switch (v->xf->rot) {
    default: ux = dx;              uy = dy;              break;
    case 1:  ux = n->h - 1 - dy;   uy = dx;              break;
    case 2:  ux = n->w - 1 - dx;   uy = n->h - 1 - dy;   break;
    case 3:  ux = dy;              uy = n->w - 1 - dx;   break;
    }
    if (v->xf->mirror & 1)
        ux = W0 - 1 - ux;
    if (v->xf->mirror & 2)
        uy = H0 - 1 - uy;
    int32_t sx = v->src.x + (int32_t)((int64_t)ux * v->src.w / W0);
    int32_t sy = v->src.y + (int32_t)((int64_t)uy * v->src.h / H0);
    if (sx < 0 || sy < 0 || sx >= v->iw || sy >= v->ih)
        return false;
    return (v->bits[(size_t)sy * v->wpr + (sx >> 5)] >> (sx & 31)) & 1;
}

/* one absolute rect against node b's box-and-ink -- the middle of the
 * plain overlaps below, factored out so a HITBOX (a pure box that may
 * sit entirely outside its node) can run the same test */
static bool rect_hits_node(surf_rect ra, const surf_node *b)
{
    int16_t bx, by;
    surf_node_abs_pos(b, &bx, &by);
    surf_rect rb = {bx, by, b->w, b->h};
    if (!surf_rect_overlaps(ra, rb))
        return false;
    ink_view vb;
    if (!node_ink(b, &vb))
        return true;
    surf_rect ix = surf_rect_intersect(ra, rb);
    for (int32_t y = 0; y < ix.h; y++)
        for (int32_t x = 0; x < ix.w; x++)
            if (ink_at(&vb, b, ix.x + x - bx, ix.y + y - by))
                return true;
    return false;
}

uint32_t surf_node_overlaps_which(const surf_node *a, const surf_node *b)
{
    if (!a || !b || surf_hitbox_count(a) == 0 ||
        (a->flags & SURF_NF_HIDDEN) || (b->flags & SURF_NF_HIDDEN) ||
        !surf_node_attached(a) || !surf_node_attached(b))
        return 0;
    /* While a node has hitboxes they ARE its collision shape: its own
     * box and ink stop mattering, and a box is allowed to sit entirely
     * outside the picture -- which is why nothing here early-outs on
     * the NODE boxes the way the plain path below does. */
    uint32_t hits = 0;
    int32_t na = surf_hitbox_count(a), nb = surf_hitbox_count(b);
    for (int32_t i = 0; i < na; i++) {
        surf_rect ra;
        if (!surf_hitbox_abs(a, i, &ra))
            continue;
        bool hit = false;
        if (nb) {
            for (int32_t j = 0; j < nb && !hit; j++) {
                surf_rect rb;
                hit = surf_hitbox_abs(b, j, &rb) && surf_rect_overlaps(ra, rb);
            }
        } else {
            hit = rect_hits_node(ra, b);
        }
        if (hit)
            hits |= 1u << i;
    }
    return hits;
}

bool surf_node_overlaps(const surf_node *a, const surf_node *b)
{
    if (!a || !b || (a->flags & SURF_NF_HIDDEN) || (b->flags & SURF_NF_HIDDEN) ||
        !surf_node_attached(a) || !surf_node_attached(b))
        return false;
    if (surf_hitbox_count(a))
        return surf_node_overlaps_which(a, b) != 0;
    if (surf_hitbox_count(b))
        return surf_node_overlaps_which(b, a) != 0;
    int16_t ax, ay, bx, by;
    surf_node_abs_pos(a, &ax, &ay);
    surf_node_abs_pos(b, &bx, &by);
    surf_rect ra = {ax, ay, a->w, a->h};
    surf_rect rb = {bx, by, b->w, b->h};
    if (!surf_rect_overlaps(ra, rb))
        return false;
    ink_view va, vb;
    bool ia = node_ink(a, &va);
    bool ib = node_ink(b, &vb);
    if (!ia && !ib)
        return true;                /* two boxes: the old answer was right */
    surf_rect ix = surf_rect_intersect(ra, rb);
    for (int32_t y = 0; y < ix.h; y++)
        for (int32_t x = 0; x < ix.w; x++) {
            int32_t px = ix.x + x, py = ix.y + y;
            if ((!ia || ink_at(&va, a, px - ax, py - ay)) &&
                (!ib || ink_at(&vb, b, px - bx, py - by)))
                return true;
        }
    return false;
}

/* SWAP THE PICTURE — see the header for the contract.
 *
 * The obvious shape is a two-line assignment, and it is wrong twice.
 * The FOOTPRINT can change (a different-sized picture, and on a strip a
 * different frame count), so the old rect has to be damaged BEFORE the
 * write and the new one after — set_src's own slow path, which is the
 * only ordering that repaints what the node is vacating. And
 * `pan_shifted` is a claim about a band shift of the OLD picture: left
 * set, the next set_src would refresh a band from a strip that is no
 * longer there.
 *
 * What deliberately does NOT change is everything the caller put on the
 * node. A transform, hitboxes, fast_pan and a filmstrip's fps and
 * transport describe how this node behaves, not what it is a picture
 * of; a swap that reset them would make the cheap thing (a state
 * change) do the expensive thing (rebuild the node) by another route,
 * which is what this exists to avoid. Hitboxes are the one to think
 * about — boxes drawn round a live slime may not fit a dead one — but
 * only the caller knows, and clearing them silently is the worse
 * failure: a node that has quietly stopped colliding looks perfect. */
bool surf_sprite_set_image(surf_node *n, const surf_image *img)
{
    if (!n || !img)
        return false;
    if (n->type == SURF_NODE_SPRITE) {
        if (n->u.sprite.img == img)
            return true;
        surf_damage_subtree(n);
        n->u.sprite.pan_shifted = false;
        n->u.sprite.img = img;
        n->u.sprite.src = (surf_rect){0, 0, img->w, img->h};
        sprite_update_size(n);
        surf_damage_subtree(n);
        return true;
    }
    if (n->type == SURF_NODE_FILMSTRIP) {
        /* A strip that cannot hold one frame would compute nframes 0,
         * and a filmstrip with no frames draws from a source rect that
         * is not in the image. surf_filmstrip_new refuses the same
         * thing at construction; this refuses it at the swap. */
        if (img->w < n->u.strip.fw || img->h < n->u.strip.fh)
            return false;
        if (n->u.strip.img == img)
            return true;
        surf_damage_subtree(n);
        n->u.strip.img = img;
        n->u.strip.per_row = (int16_t)(img->w / n->u.strip.fw);
        n->u.strip.nframes = (int16_t)(n->u.strip.per_row *
                                       (img->h / n->u.strip.fh));
        if (n->u.strip.frame >= n->u.strip.nframes)
            n->u.strip.frame = (int16_t)(n->u.strip.nframes - 1);
        sprite_update_size(n);
        surf_damage_subtree(n);
        return true;
    }
    return false;
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
                         n->opa == 255 && surf_node_attached(n) &&
                         !surf_node_effectively_hidden(n);
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
    /* opa < 255 rules out streaming: a band shift moves pixels that are
     * ALREADY the composite of this node over what was behind it, and
     * blending the node again over its own result blends twice. */
    bool can_fast = pan_only && n->u.sprite.fast_pan && surf_g.hal->band_shift &&
                    n->u.sprite.img->opaque && n->opa == 255 &&
                    n->u.sprite.xf.scale_q16 == SURF_ONE && n->u.sprite.xf.rot == 0 &&
                    n->u.sprite.xf.mirror == 0 && surf_node_attached(n) &&
                    !surf_node_effectively_hidden(n) &&
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
             * into a full-band repaint. surf_dirty_add even-aligns every
             * rect (the P4's SRM wedge), so the vertical sliver is
             * aligned HERE and the horizontal one stops at its aligned
             * edge — an odd dx handed down raw would widen the vertical
             * sliver into the horizontal one and coalesce the L into
             * the very full-band repaint this path exists to avoid */
            int16_t vx0 = band.x, vx1 = band.x;
            if (adx) {
                vx0 = dx > 0 ? (int16_t)(band.x + band.w - adx) : band.x;
                vx1 = (int16_t)((vx0 + adx + 1) & ~1);
                vx0 &= (int16_t)~1;
                surf_dirty_add(&surf_g.dirty, (surf_rect){
                    vx0, band.y, (int16_t)(vx1 - vx0), band.h});
            }
            if (ady) {
                int16_t hx0 = band.x, hx1 = (int16_t)(band.x + band.w);
                if (adx) {
                    if (dx > 0) hx1 = vx0;
                    else        hx0 = vx1;
                }
                if (hx1 > hx0)
                    surf_dirty_add(&surf_g.dirty, (surf_rect){
                        hx0,
                        dy > 0 ? (int16_t)(band.y + band.h - ady) : band.y,
                        (int16_t)(hx1 - hx0), ady});
            }
            /* everything painted over the band was smeared by the
             * shift — including overlays in another branch of the tree,
             * which is where a host's chrome lives */
            surf_damage_above(n, band, adx, ady);
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
    surf_tween_drop(n, SURF_TW_SCALE);   /* a direct write wins */
    xform_apply(n, scale_q16, rot, mirror);
}

/* SPRITES AND FILMSTRIPS BOTH, and that is the whole of this change.
 * It used to return for anything that was not a SPRITE — so scaling an
 * animation did nothing at all, silently, and read back as 1.0 — which
 * is the worst failure a setter has, because the code looks right and
 * the picture never moves. A filmstrip is a sprite that picks its
 * source from a frame index; there was never a reason it could not be
 * scaled, only a type test that said so. */
/* The transform write, cancelling nothing — what a running SCALE tween
 * calls. The public setter below kills that tween first. */
static void xform_apply(surf_node *n, int32_t scale_q16, uint8_t rot,
                        uint8_t mirror)
{
    surf_xform *xf = surf_node_xform(n);
    if (!xf || scale_q16 <= 0)
        return;
    /* the PPA SRM range; keep every backend honest about it */
    if (scale_q16 < SURF_ONE / 16) scale_q16 = SURF_ONE / 16;
    if (scale_q16 > SURF_ONE * 16) scale_q16 = SURF_ONE * 16;
    rot &= 3;
    mirror &= 3;
    if (scale_q16 == xf->scale_q16 && rot == xf->rot && mirror == xf->mirror)
        return;
    surf_damage_subtree(n);
    xf->scale_q16 = scale_q16;
    xf->rot = rot;
    xf->mirror = mirror;
    sprite_update_size(n);
    surf_damage_subtree(n);
}

bool surf_node_can_xform(const surf_node *n)
{
    return surf_node_xform((surf_node *)n) != NULL;
}

int32_t surf_sprite_scale(const surf_node *n)
{
    const surf_xform *xf = surf_node_xform((surf_node *)n);
    return xf ? xf->scale_q16 : SURF_ONE;
}

uint8_t surf_sprite_rot(const surf_node *n)
{
    const surf_xform *xf = surf_node_xform((surf_node *)n);
    return xf ? xf->rot : 0;
}

uint8_t surf_sprite_mirror(const surf_node *n)
{
    const surf_xform *xf = surf_node_xform((surf_node *)n);
    return xf ? xf->mirror : 0;
}

surf_node *surf_node_parent(surf_node *n)
{
    return n ? n->parent : NULL;
}

/* ---- hitboxes -------------------------------------------------------
 * Storage is a plain malloc'd array per node -- add/remove are events,
 * never the frame path -- freed in node_free. Offsets live in the
 * UNROTATED source frame; surf_hitbox_abs is the FORWARD map of the
 * inverse ink_at() runs, derived case by case from it so the two cannot
 * disagree about what a quarter turn means. */

static surf_hitbox **hb_slot(surf_node *n, uint8_t **cnt)
{
    if (!n)
        return NULL;
    if (n->type == SURF_NODE_SPRITE) {
        *cnt = &n->u.sprite.hbn;
        return &n->u.sprite.hb;
    }
    if (n->type == SURF_NODE_FILMSTRIP) {
        *cnt = &n->u.strip.hbn;
        return &n->u.strip.hb;
    }
    return NULL;
}

int32_t surf_hitbox_count(const surf_node *n)
{
    uint8_t *cnt;
    surf_hitbox **hb = hb_slot((surf_node *)n, &cnt);
    return hb ? *cnt : 0;
}

int32_t surf_hitbox_add(surf_node *n, int16_t x, int16_t y,
                        int16_t w, int16_t h)
{
    uint8_t *cnt;
    surf_hitbox **hb = hb_slot(n, &cnt);
    if (!hb || *cnt >= SURF_MAX_HITBOXES || w <= 0 || h <= 0)
        return -1;
    surf_hitbox *grown = realloc(*hb, ((size_t)*cnt + 1) * sizeof **hb);
    if (!grown)
        return -1;
    *hb = grown;
    grown[*cnt] = (surf_hitbox){x, y, w, h};
    return (*cnt)++;
}

bool surf_hitbox_set(surf_node *n, int32_t i, int16_t x, int16_t y,
                     int16_t w, int16_t h)
{
    uint8_t *cnt;
    surf_hitbox **hb = hb_slot(n, &cnt);
    if (!hb || i < 0 || i >= *cnt || w <= 0 || h <= 0)
        return false;
    (*hb)[i] = (surf_hitbox){x, y, w, h};
    return true;
}

bool surf_hitbox_get(const surf_node *n, int32_t i, surf_hitbox *out)
{
    uint8_t *cnt;
    surf_hitbox **hb = hb_slot((surf_node *)n, &cnt);
    if (!hb || i < 0 || i >= *cnt)
        return false;
    *out = (*hb)[i];
    return true;
}

bool surf_hitbox_remove(surf_node *n, int32_t i)
{
    uint8_t *cnt;
    surf_hitbox **hb = hb_slot(n, &cnt);
    if (!hb || i < 0 || i >= *cnt)
        return false;
    for (int32_t k = i; k < *cnt - 1; k++)
        (*hb)[k] = (*hb)[k + 1];
    (*cnt)--;
    if (*cnt == 0) {
        free(*hb);
        *hb = NULL;
    }
    return true;
}

/* which cell the offsets are authored against */
static bool hb_cell(const surf_node *n, int32_t *cw, int32_t *ch)
{
    if (n->type == SURF_NODE_SPRITE) {
        *cw = n->u.sprite.src.w;
        *ch = n->u.sprite.src.h;
    } else {
        *cw = n->u.strip.fw;
        *ch = n->u.strip.fh;
    }
    return *cw > 0 && *ch > 0;
}

bool surf_hitbox_abs(const surf_node *n, int32_t i, surf_rect *out)
{
    surf_hitbox b;
    if (!surf_hitbox_get(n, i, &b) || !out)
        return false;
    int32_t cw, ch;
    if (!hb_cell(n, &cw, &ch))
        return false;
    const surf_xform *xf = surf_node_xform((surf_node *)n);
    /* the PRE-rotation footprint, i.e. the scaled frame (ink_at's W0/H0) */
    int32_t W0 = (xf->rot & 1) ? n->h : n->w;
    int32_t H0 = (xf->rot & 1) ? n->w : n->h;
    if (W0 <= 0 || H0 <= 0)
        return false;
    /* scale the source-frame box onto the footprint, end minus start so
     * adjacent boxes stay adjacent under a fractional scale */
    int32_t X0 = (int32_t)((int64_t)b.x * W0 / cw);
    int32_t X1 = (int32_t)((int64_t)(b.x + b.w) * W0 / cw);
    int32_t Y0 = (int32_t)((int64_t)b.y * H0 / ch);
    int32_t Y1 = (int32_t)((int64_t)(b.y + b.h) * H0 / ch);
    int32_t X = X0, Y = Y0, Wb = X1 - X0, Hb = Y1 - Y0;
    if (xf->mirror & 1)
        X = W0 - X - Wb;
    if (xf->mirror & 2)
        Y = H0 - Y - Hb;
    int32_t dx, dy, dw, dh;
    switch (xf->rot) {
    default: dx = X;            dy = Y;            dw = Wb; dh = Hb; break;
    case 1:  dx = Y;            dy = W0 - X - Wb;  dw = Hb; dh = Wb; break;
    case 2:  dx = W0 - X - Wb;  dy = H0 - Y - Hb;  dw = Wb; dh = Hb; break;
    case 3:  dx = H0 - Y - Hb;  dy = X;            dw = Hb; dh = Wb; break;
    }
    int16_t ax, ay;
    surf_node_abs_pos(n, &ax, &ay);
    *out = (surf_rect){(int16_t)(ax + dx), (int16_t)(ay + dy),
                       (int16_t)dw, (int16_t)dh};
    return true;
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

/* Everything painted ABOVE a hal-shifted rect had its pixels dragged
 * along by the shift and has to be repainted where it actually is.
 *
 * "Above" is the whole paint order after this node — NOT just its later
 * siblings, which is what the layer used to walk. An overlay living in
 * another branch of the tree is exactly the case that breaks: tulip's
 * task bar and its console scrollbar are siblings of an app's GROUP, not
 * of the scrolling node inside it, so they smeared across the screen at
 * whatever rate the thing under them was scrolling.
 *
 * The node's own subtree is skipped: it moves with the shift. */
static bool damage_above_walk(surf_node *cur, const surf_node *stop, bool after,
                              surf_rect area, int16_t gx, int16_t gy)
{
    if (cur == stop)
        return true;
    /* A hidden subtree paints nothing, so the shift dragged none of its
     * pixels and none of them need repainting — the same early out
     * collect() takes. Testing the flag per node instead sent every
     * child of a backgrounded app (tulip hides an app's GROUP, not its
     * children — 1100 nodes for one of them) through abs_pos and into
     * the dirty list, where SURF_MAX_DIRTY entries degrade to a bounding
     * union and a scroll costs a full-screen compose.
     *
     * `stop` is never in here: every shift gate now refuses to shift
     * from inside a hidden subtree (surf_node_effectively_hidden), so a
     * branch that paints nothing cannot be the one that moved pixels. */
    if (cur->flags & SURF_NF_HIDDEN)
        return after;
    if (after && cur->w > 0 && cur->h > 0) {
        int16_t ax, ay;
        surf_node_abs_pos(cur, &ax, &ay);
        surf_rect r = {(int16_t)(ax - gx), (int16_t)(ay - gy),
                       (int16_t)(cur->w + 2 * gx), (int16_t)(cur->h + 2 * gy)};
        r = surf_rect_intersect(r, area);
        if (!surf_rect_empty(r))
            surf_dirty_add(&surf_g.dirty, r);
    }
    for (surf_node *c = cur->first; c; c = c->next)
        after = damage_above_walk(c, stop, after, area, gx, gy);
    return after;
}

void surf_damage_above(const surf_node *n, surf_rect area, int16_t gx, int16_t gy)
{
    if (surf_g.root && n)
        damage_above_walk(surf_g.root, n, false, area, gx, gy);
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
                         n->opa == 255 && surf_node_attached(n) &&
                         !surf_node_effectively_hidden(n);
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
                    n->u.layer.strip->opaque && n->opa == 255 &&
                    surf_node_attached(n) &&
                    !surf_node_effectively_hidden(n) &&
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

    /* anything drawn over the band just got smeared by the shift:
     * repaint it, expanded by the shift so the ghost goes too */
    surf_damage_above(n, band, adx, 0);
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

/* PLAY IT, or don't. fps 0 hands the frame back to the caller, which is
 * what a cel editor wants and what a game driving a walk cycle off its
 * own physics wants; anything else advances from surf_tick and wraps.
 *
 * A COUNT of animating strips, so the scan below costs nothing at all in
 * the overwhelmingly common case of none. Without it every tick would
 * walk the whole 4096-node pool to discover that nothing is playing.
 * `strip_active` is the ONE definition of what counts — fps set AND not
 * stopped — so set_fps, set_playing and node_free cannot disagree about
 * the bookkeeping. */
static bool strip_active(const surf_node *n)
{
    return n->u.strip.fps_q16 != 0 && !n->u.strip.paused;
}

void surf_filmstrip_set_fps(surf_node *n, int32_t fps_q16)
{
    if (!n || n->type != SURF_NODE_FILMSTRIP)
        return;
    if (fps_q16 < 0)
        fps_q16 = 0;
    bool was = strip_active(n);
    n->u.strip.fps_q16 = fps_q16;
    if (strip_active(n) != was)
        surf_g.playing += was ? -1 : 1;
    n->u.strip.due_us = 0;       /* re-anchor: start the clock from now */
}

int32_t surf_filmstrip_fps(const surf_node *n)
{
    return (n && n->type == SURF_NODE_FILMSTRIP) ? n->u.strip.fps_q16 : 0;
}

/* THE TRANSPORT, and it is a second axis on purpose: stop() freezes the
 * strip KEEPING its fps, so play() resumes at the speed it had — which
 * zeroing fps cannot do, and which is the pair a caller reaches for
 * before learning that fps 0 means "the frame is mine now". play(0) on
 * a strip whose fps is 0 stays still: there is no speed to resume. */
void surf_filmstrip_set_playing(surf_node *n, bool on)
{
    if (!n || n->type != SURF_NODE_FILMSTRIP)
        return;
    bool was = strip_active(n);
    n->u.strip.paused = !on;
    if (strip_active(n) != was)
        surf_g.playing += was ? -1 : 1;
    n->u.strip.due_us = 0;       /* resume from NOW, not from a stale due */
}

bool surf_filmstrip_playing(const surf_node *n)
{
    return n && n->type == SURF_NODE_FILMSTRIP && strip_active(n);
}

void surf_filmstrip_tick(void)
{
    if (surf_g.playing <= 0 || !surf_g.hal || !surf_g.hal->now_us)
        return;
    uint64_t now = surf_g.hal->now_us();
    for (int i = 0; i < surf_g.pool_cap; i++) {
        surf_node *n = &surf_g.pool[i];
        if (n->type != SURF_NODE_FILMSTRIP || !strip_active(n))
            continue;
        if (n->u.strip.nframes < 2)
            continue;
        uint64_t period = (uint64_t)((int64_t)1000000 * SURF_ONE
                                     / n->u.strip.fps_q16);
        if (!period)
            period = 1;
        if (!n->u.strip.due_us) {
            n->u.strip.due_us = now + period;
            continue;
        }
        if (now < n->u.strip.due_us)
            continue;
        /* Late frames are DROPPED, not replayed. A tab that was hidden
         * for a minute would otherwise flip through thousands of cels to
         * catch up on a cycle nobody watched. */
        int32_t steps = (int32_t)((now - n->u.strip.due_us) / period) + 1;
        if (steps > n->u.strip.nframes)
            steps = steps % n->u.strip.nframes;
        n->u.strip.due_us = now + period;
        int16_t f = (int16_t)((n->u.strip.frame + steps)
                              % n->u.strip.nframes);
        if (f != n->u.strip.frame) {
            n->u.strip.frame = f;
            surf_damage_subtree(n);
        }
    }
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
