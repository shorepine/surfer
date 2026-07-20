#include "surf_internal.h"

/* Per dirty rect: walk front-to-back collecting visible leaves, stopping at
 * the first opaque leaf that covers the whole rect; then paint the collected
 * list back-to-front (DESIGN.md §2.3). Everything below the stop node is
 * never touched — that's the occlusion win, no pixel reads needed. */

static int paint_n;

static bool node_opaque(const surf_node *n)
{
    switch (n->type) {
    case SURF_NODE_RECT:      return true;
    case SURF_NODE_SPRITE:    return n->u.sprite.img->opaque;
    case SURF_NODE_LAYER:     return n->u.layer.strip->opaque;
    case SURF_NODE_FILMSTRIP: return n->u.strip.img->opaque;
    case SURF_NODE_NINEPATCH: return n->u.nine.img->opaque;
    case SURF_NODE_TEXTGRID:  return true;  /* bg+glyph fills every pixel */
    default:                  return false;
    }
}

/* Returns true once the dirty rect is fully covered by an opaque leaf. */
static bool collect(surf_node *n, int16_t px, int16_t py, surf_rect clip, surf_rect dr)
{
    if (n->flags & SURF_NF_HIDDEN)
        return false;
    int16_t ax = (int16_t)(px + n->x), ay = (int16_t)(py + n->y);

    if (n->type == SURF_NODE_GROUP || n->type == SURF_NODE_SCROLLVIEW) {
        if (n->type == SURF_NODE_SCROLLVIEW || (n->flags & SURF_NF_CLIP)) {
            clip = surf_rect_intersect(clip, (surf_rect){ax, ay, n->w, n->h});
            if (surf_rect_empty(clip))
                return false;
        }
        if (n->type == SURF_NODE_SCROLLVIEW) {
            ax = (int16_t)(ax - (n->u.scroll.off_x >> 16));
            ay = (int16_t)(ay - (n->u.scroll.off_y >> 16));
        }
        for (surf_node *c = n->last; c; c = c->prev)
            if (collect(c, ax, ay, clip, dr))
                return true;
        return false;
    }

    surf_rect bounds = {ax, ay, n->w, n->h};
    surf_rect vis = surf_rect_intersect(bounds, clip);
    if (surf_rect_empty(vis))
        return false;

    surf_g.plist[paint_n++] = (surf_paint_ent){n, ax, ay, vis};
    return node_opaque(n) && surf_rect_covers(bounds, dr);
}

static void image_op(const surf_image *img, surf_rect src, surf_point dst)
{
    if (img->opaque)
        surf_g.hal->blit(img, src, dst);
    else
        surf_g.hal->blend(img, src, dst, 255);
}

/* Blit src repeatedly across dst, each tile clipped to vis. This is how
 * 9-patch edges/centers stretch without scale_blit (cut from the v1 frame
 * path, DESIGN.md §5.4). */
static void tile_blit(const surf_image *img, surf_rect src, surf_rect dst, surf_rect vis)
{
    if (surf_rect_empty(src) || surf_rect_empty(dst))
        return;
    surf_rect clip = surf_rect_intersect(dst, vis);
    if (surf_rect_empty(clip))
        return;
    for (int y = dst.y; y < dst.y + dst.h; y += src.h) {
        for (int x = dst.x; x < dst.x + dst.w; x += src.w) {
            surf_rect t = {(int16_t)x, (int16_t)y, src.w, src.h};
            surf_rect v = surf_rect_intersect(t, clip);
            if (surf_rect_empty(v))
                continue;
            surf_rect s = {
                (int16_t)(src.x + (v.x - x)), (int16_t)(src.y + (v.y - y)), v.w, v.h,
            };
            image_op(img, s, (surf_point){v.x, v.y});
        }
    }
}

static void paint_ninepatch(const surf_paint_ent *e)
{
    const surf_node *n = e->n;
    const surf_image *img = n->u.nine.img;
    int l = n->u.nine.l, t = n->u.nine.t, r = n->u.nine.r, b = n->u.nine.b;
    int sx[4] = {0, l, img->w - r, img->w};
    int sy[4] = {0, t, img->h - b, img->h};
    int dx[4] = {e->ax, e->ax + l, e->ax + n->w - r, e->ax + n->w};
    int dy[4] = {e->ay, e->ay + t, e->ay + n->h - b, e->ay + n->h};

    for (int ry = 0; ry < 3; ry++) {
        for (int rx = 0; rx < 3; rx++) {
            surf_rect s = {(int16_t)sx[rx], (int16_t)sy[ry],
                           (int16_t)(sx[rx + 1] - sx[rx]), (int16_t)(sy[ry + 1] - sy[ry])};
            surf_rect d = {(int16_t)dx[rx], (int16_t)dy[ry],
                           (int16_t)(dx[rx + 1] - dx[rx]), (int16_t)(dy[ry + 1] - dy[ry])};
            tile_blit(img, s, d, e->vis);
        }
    }
}

static void paint(const surf_paint_ent *e)
{
    surf_node *n = e->n;

    switch (n->type) {
    case SURF_NODE_RECT:
        surf_g.hal->fill(e->vis, n->u.rect.color);
        return;
    case SURF_NODE_SPRITE:
        if (n->u.sprite.scale_q16 != SURF_ONE || n->u.sprite.rot != 0 ||
            n->u.sprite.mirror != 0) {
            /* transformed: the hal draws from the full source into the
             * full footprint, clipped to vis (partial-src arithmetic
             * doesn't survive scaling, rotation or mirroring) */
            surf_g.hal->xform_blend(n->u.sprite.img, n->u.sprite.src,
                                    (surf_rect){e->ax, e->ay, n->w, n->h},
                                    e->vis, n->u.sprite.rot,
                                    n->u.sprite.mirror);
            return;
        }
        image_op(n->u.sprite.img, (surf_rect){
                     (int16_t)(n->u.sprite.src.x + (e->vis.x - e->ax)),
                     (int16_t)(n->u.sprite.src.y + (e->vis.y - e->ay)),
                     e->vis.w, e->vis.h,
                 }, (surf_point){e->vis.x, e->vis.y});
        return;
    case SURF_NODE_LAYER: {
        /* wrap-scrolling strip: vis maps to <=2 source segments */
        const surf_image *strip = n->u.layer.strip;
        int32_t sw = strip->w;
        int32_t sx = ((n->u.layer.off_q16 >> 16) + (e->vis.x - e->ax)) % sw;
        if (sx < 0)
            sx += sw;
        int32_t remaining = e->vis.w;
        int16_t dx = e->vis.x;
        while (remaining > 0) {
            int32_t seg = sw - sx < remaining ? sw - sx : remaining;
            image_op(strip, (surf_rect){
                         (int16_t)sx, (int16_t)(e->vis.y - e->ay),
                         (int16_t)seg, e->vis.h,
                     }, (surf_point){dx, e->vis.y});
            dx = (int16_t)(dx + seg);
            remaining -= seg;
            sx = 0;
        }
        return;
    }
    case SURF_NODE_FILMSTRIP: {
        int fx = (n->u.strip.frame % n->u.strip.per_row) * n->u.strip.fw;
        int fy = (n->u.strip.frame / n->u.strip.per_row) * n->u.strip.fh;
        image_op(n->u.strip.img, (surf_rect){
                     (int16_t)(fx + (e->vis.x - e->ax)),
                     (int16_t)(fy + (e->vis.y - e->ay)),
                     e->vis.w, e->vis.h,
                 }, (surf_point){e->vis.x, e->vis.y});
        return;
    }
    case SURF_NODE_NINEPATCH:
        paint_ninepatch(e);
        return;
    case SURF_NODE_TEXT:
        surf_text_paint(e);
        return;
    case SURF_NODE_TEXTINPUT:
        surf_textinput_paint(e);
        return;
    case SURF_NODE_TEXTGRID:
        surf_textgrid_paint(e);
        return;
    }
}

void surf_compose(void)
{
    surf_dirty *d = &surf_g.dirty;
    if (d->n == 0)
        return;

    for (int i = 0; i < d->n; i++) {
        surf_rect dr = d->r[i];
        paint_n = 0;
        bool covered = collect(surf_g.root, 0, 0, dr, dr);
        if (!covered)
            surf_g.hal->fill(dr, surf_g.bg);
        for (int j = paint_n - 1; j >= 0; j--)
            paint(&surf_g.plist[j]);
    }

    surf_g.hal->present(d->r, d->n);
    surf_dirty_reset(d, (surf_rect){0, 0, surf_g.w, surf_g.h});
}
