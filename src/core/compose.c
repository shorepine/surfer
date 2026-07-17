#include "surf_internal.h"

/* Per dirty rect: walk front-to-back collecting visible leaves, stopping at
 * the first opaque leaf that covers the whole rect; then paint the collected
 * list back-to-front (DESIGN.md §2.3). Everything below the stop node is
 * never touched — that's the occlusion win, no pixel reads needed. */

static int paint_n;

static bool node_opaque(const surf_node *n)
{
    if (n->type == SURF_NODE_RECT)
        return true;
    return n->type == SURF_NODE_SPRITE && n->u.sprite.img->opaque;
}

/* Returns true once the dirty rect is fully covered by an opaque leaf. */
static bool collect(surf_node *n, int16_t px, int16_t py, surf_rect clip, surf_rect dr)
{
    if (n->flags & SURF_NF_HIDDEN)
        return false;
    int16_t ax = (int16_t)(px + n->x), ay = (int16_t)(py + n->y);

    if (n->type == SURF_NODE_GROUP) {
        if (n->flags & SURF_NF_CLIP) {
            clip = surf_rect_intersect(clip, (surf_rect){ax, ay, n->w, n->h});
            if (surf_rect_empty(clip))
                return false;
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

static void paint(const surf_paint_ent *e)
{
    const surf_hal *hal = surf_g.hal;
    surf_node *n = e->n;

    if (n->type == SURF_NODE_RECT) {
        hal->fill(e->vis, n->u.rect.color);
        return;
    }

    const surf_image *img = n->u.sprite.img;
    surf_rect src = {
        (int16_t)(n->u.sprite.src.x + (e->vis.x - e->ax)),
        (int16_t)(n->u.sprite.src.y + (e->vis.y - e->ay)),
        e->vis.w, e->vis.h,
    };
    surf_point dst = {e->vis.x, e->vis.y};
    if (img->opaque)
        hal->blit(img, src, dst);
    else
        hal->blend(img, src, dst, 255);
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
