#include "surf_internal.h"

/* Front-to-back walk honoring clip (DESIGN.md §2.6). Groups are transparent
 * to hits unless they carry a handler and a size — that makes hot areas and
 * scrims possible. A scrollview hits itself when no child does, so empty
 * space inside it is draggable. */
static surf_node *hit(surf_node *n, int16_t px, int16_t py, int16_t x, int16_t y)
{
    if (n->flags & SURF_NF_HIDDEN)
        return NULL;
    int16_t ax = (int16_t)(px + n->x), ay = (int16_t)(py + n->y);

    if (n->type == SURF_NODE_GROUP || n->type == SURF_NODE_SCROLLVIEW) {
        bool clipped = n->type == SURF_NODE_SCROLLVIEW || (n->flags & SURF_NF_CLIP);
        bool inside = surf_rect_contains((surf_rect){ax, ay, n->w, n->h}, x, y);
        if (clipped && !inside)
            return NULL;
        int16_t cx = ax, cy = ay;
        if (n->type == SURF_NODE_SCROLLVIEW) {
            cx = (int16_t)(ax - (n->u.scroll.off_x >> 16));
            cy = (int16_t)(ay - (n->u.scroll.off_y >> 16));
        }
        for (surf_node *c = n->last; c; c = c->prev) {
            surf_node *h = hit(c, cx, cy, x, y);
            if (h)
                return h;
        }
        if (inside && (n->type == SURF_NODE_SCROLLVIEW || n->on_touch))
            return n;
        return NULL;
    }

    if (surf_rect_contains((surf_rect){ax, ay, n->w, n->h}, x, y))
        return n;
    return NULL;
}

surf_node *surf_hit_test(int16_t x, int16_t y)
{
    if (!surf_g.root)
        return NULL;
    return hit(surf_g.root, 0, 0, x, y);
}
