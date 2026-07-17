#include "surf_internal.h"

/* Front-to-back walk honoring clip (DESIGN.md §2.6). Groups are transparent
 * to hits; only leaves are returned. */
static surf_node *hit(surf_node *n, int16_t px, int16_t py, int16_t x, int16_t y)
{
    if (n->flags & SURF_NF_HIDDEN)
        return NULL;
    int16_t ax = (int16_t)(px + n->x), ay = (int16_t)(py + n->y);

    if (n->type == SURF_NODE_GROUP) {
        if ((n->flags & SURF_NF_CLIP) &&
            !surf_rect_contains((surf_rect){ax, ay, n->w, n->h}, x, y))
            return NULL;
        for (surf_node *c = n->last; c; c = c->prev) {
            surf_node *h = hit(c, ax, ay, x, y);
            if (h)
                return h;
        }
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
