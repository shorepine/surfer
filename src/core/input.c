#include "surf_internal.h"

/* Touch → hit test → nearest ancestor with a handler. That node holds the
 * pointer from DOWN to UP, so drags keep tracking after the finger leaves
 * its rect (DESIGN.md §2.6). scrollview's gesture-steal threshold arrives
 * in M4 on top of this. */

static surf_node *handler_for(surf_node *n)
{
    while (n && !n->on_touch)
        n = n->parent;
    return n;
}

void surf_input_dispatch(const surf_touch *t)
{
    switch (t->phase) {
    case SURF_TOUCH_DOWN: {
        surf_node *h = handler_for(surf_hit_test(t->x, t->y));
        surf_g.capture = h;
        if (h)
            h->on_touch(h, t, h->touch_user);
        break;
    }
    case SURF_TOUCH_MOVE:
        if (surf_g.capture)
            surf_g.capture->on_touch(surf_g.capture, t, surf_g.capture->touch_user);
        break;
    case SURF_TOUCH_UP: {
        surf_node *h = surf_g.capture;
        surf_g.capture = NULL;
        if (h)
            h->on_touch(h, t, h->touch_user);
        break;
    }
    }
}
