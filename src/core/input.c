#include "surf_internal.h"

/* Touch → hit test → nearest ancestor that wants the gesture: a node with
 * a handler, or a scrollable scrollview (whichever comes first walking
 * up). The winner holds the pointer from DOWN to UP (DESIGN.md §2.6).
 *
 * When a handler wins inside a scrollview, the scrollview waits: once the
 * finger travels past STEAL_PX along an axis it can scroll, it steals the
 * gesture — the handler gets a synthetic UP and the drag becomes a
 * scroll. Handlers that own their drags (sliders, knobs, textinput
 * selection) set SURF_NF_GRAB and are never stolen from. */

#define STEAL_PX 8

static void deliver(surf_node *n, const surf_touch *t)
{
    if (n->type == SURF_NODE_SCROLLVIEW && !n->on_touch)
        surf_scroll_touch(n, t);
    else if (n->on_touch)
        n->on_touch(n, t, n->touch_user);
}

void surf_input_dispatch(const surf_touch *t)
{
    switch (t->phase) {
    case SURF_TOUCH_DOWN: {
        surf_g.capture = NULL;
        surf_g.steal_sv = NULL;
        surf_g.down_x = t->x;
        surf_g.down_y = t->y;

        surf_node *n = surf_hit_test(t->x, t->y);
        for (; n; n = n->parent) {
            if (n->on_touch)
                break;
            if (n->type == SURF_NODE_SCROLLVIEW &&
                (surf_scroll_can_x(n) || surf_scroll_can_y(n)))
                break;
        }
        if (!n)
            return;
        surf_g.capture = n;
        if (n->type == SURF_NODE_SCROLLVIEW && !n->on_touch) {
            surf_scroll_begin(n, t);
            return;
        }
        n->on_touch(n, t, n->touch_user);
        if (!(n->flags & SURF_NF_GRAB)) {
            for (surf_node *p = n->parent; p; p = p->parent) {
                if (p->type == SURF_NODE_SCROLLVIEW &&
                    (surf_scroll_can_x(p) || surf_scroll_can_y(p))) {
                    surf_g.steal_sv = p;
                    break;
                }
            }
        }
        break;
    }
    case SURF_TOUCH_MOVE: {
        surf_node *c = surf_g.capture;
        if (!c)
            return;
        surf_node *sv = surf_g.steal_sv;
        if (sv) {
            int dx = t->x - surf_g.down_x, dy = t->y - surf_g.down_y;
            int adx = dx < 0 ? -dx : dx, ady = dy < 0 ? -dy : dy;
            bool steal = (ady >= adx) ? (ady > STEAL_PX && surf_scroll_can_y(sv))
                                      : (adx > STEAL_PX && surf_scroll_can_x(sv));
            if (steal) {
                surf_touch up = {t->x, t->y, SURF_TOUCH_UP};
                deliver(c, &up);
                surf_g.capture = sv;
                surf_g.steal_sv = NULL;
                surf_scroll_begin(sv, t);
                return;
            }
        }
        deliver(c, t);
        break;
    }
    case SURF_TOUCH_UP: {
        surf_node *c = surf_g.capture;
        surf_g.capture = NULL;
        surf_g.steal_sv = NULL;
        if (c)
            deliver(c, t);
        break;
    }
    }
}
