/* Checkbox: a 2-frame filmstrip (unchecked, checked). Tap toggles;
 * releasing outside the box cancels, and a scrollview may steal the
 * gesture — a checkbox tap is exactly the thing scrolling wins over. */
#include <stdlib.h>

#include "surfer.h"

struct surf_checkbox {
    surf_node     *root, *strip;
    int16_t        w, h;
    bool           checked;
    surf_change_cb cb;
    void          *user;
};

static void checkbox_touch(surf_node *n, const surf_touch *t, void *user)
{
    (void)n;
    surf_checkbox *c = user;
    if (t->phase != SURF_TOUCH_UP)
        return;
    int16_t ax, ay;
    surf_node_abs_pos(c->root, &ax, &ay);
    if (t->x < ax || t->x >= ax + c->w || t->y < ay || t->y >= ay + c->h)
        return;  /* released outside: cancel */
    c->checked = !c->checked;
    surf_filmstrip_set_frame(c->strip, c->checked ? 1 : 0);
    if (c->cb)
        c->cb(c->checked ? SURF_ONE : 0, c->user);
}

surf_checkbox *surf_checkbox_new(surf_node *parent, int16_t x, int16_t y,
                                 const surf_checkbox_style *style)
{
    if (!parent || !style || !style->strip)
        return NULL;
    surf_checkbox *c = calloc(1, sizeof *c);
    if (!c)
        return NULL;
    c->w = style->frame_w;
    c->h = style->frame_h;
    c->root = surf_group_new(x, y);
    c->strip = surf_filmstrip_new(style->strip, style->frame_w, style->frame_h, 0, 0);
    if (!c->root || !c->strip) {
        surf_node_destroy(c->root);
        surf_node_destroy(c->strip);
        free(c);
        return NULL;
    }
    surf_node_add(c->root, c->strip);
    surf_node_set_on_touch(c->root, checkbox_touch, c);
    surf_node_add(parent, c->root);
    return c;
}

void surf_checkbox_destroy(surf_checkbox *c)
{
    if (!c)
        return;
    surf_node_destroy(c->root);
    free(c);
}

surf_node *surf_checkbox_node(surf_checkbox *c) { return c ? c->root : NULL; }

bool surf_checkbox_checked(const surf_checkbox *c) { return c && c->checked; }

void surf_checkbox_set_checked(surf_checkbox *c, bool on)
{
    if (!c || c->checked == on)
        return;
    c->checked = on;
    surf_filmstrip_set_frame(c->strip, on ? 1 : 0);
}

void surf_checkbox_on_change(surf_checkbox *c, surf_change_cb cb, void *user)
{
    if (!c)
        return;
    c->cb = cb;
    c->user = user;
}
