/* Push button: two 9-patch states (normal/pressed) + a centered label.
 * Fires on release inside; releasing outside cancels, and a scrollview
 * may steal the gesture — a button tap is exactly what scrolling should
 * win over (like checkbox). */
#include <stdlib.h>

#include "surfer.h"

struct surf_button {
    surf_node   *root, *up, *down, *label;
    int16_t      w, h;
    surf_change_cb cb;
    void        *user;
};

static void set_pressed(surf_button *b, bool on)
{
    surf_node_set_hidden(b->up, on);
    surf_node_set_hidden(b->down, !on);
}

static void button_touch(surf_node *n, const surf_touch *t, void *user)
{
    (void)n;
    surf_button *b = user;
    int16_t ax, ay;
    surf_node_abs_pos(b->root, &ax, &ay);
    bool inside = t->x >= ax && t->x < ax + b->w && t->y >= ay && t->y < ay + b->h;

    switch (t->phase) {
    case SURF_TOUCH_DOWN:
        set_pressed(b, true);
        break;
    case SURF_TOUCH_MOVE:
        set_pressed(b, inside);
        break;
    case SURF_TOUCH_UP:
        set_pressed(b, false);
        if (inside && b->cb)
            b->cb(SURF_ONE, b->user);
        break;
    }
}

surf_button *surf_button_new(surf_node *parent, int16_t x, int16_t y,
                             int16_t w, int16_t h, const surf_button_style *style,
                             const char *label)
{
    if (!parent || !style || !style->normal || !style->pressed || !style->font)
        return NULL;
    surf_button *b = calloc(1, sizeof *b);
    if (!b)
        return NULL;
    b->w = w;
    b->h = h;
    b->root = surf_group_new(x, y);
    b->up = surf_ninepatch_new(style->normal, 0, 0, w, h, style->inset,
                               style->inset, style->inset, style->inset);
    b->down = surf_ninepatch_new(style->pressed, 0, 0, w, h, style->inset,
                                 style->inset, style->inset, style->inset);
    b->label = surf_text_new(style->font, label ? label : "", 0, 0,
                             style->text_color);
    if (!b->root || !b->up || !b->down || !b->label) {
        surf_node_destroy(b->root);
        surf_node_destroy(b->up);
        surf_node_destroy(b->down);
        surf_node_destroy(b->label);
        free(b);
        return NULL;
    }
    surf_node_set_hidden(b->down, true);
    /* center the label via the wrap box */
    surf_text_set_wrap(b->label, w);
    surf_text_set_align(b->label, SURF_ALIGN_CENTER);
    surf_point ls = surf_node_size(b->label);
    surf_node_set_pos(b->label, 0, (int16_t)((h - ls.y) / 2));
    surf_node_add(b->root, b->up);
    surf_node_add(b->root, b->down);
    surf_node_add(b->root, b->label);
    surf_node_set_on_touch(b->root, button_touch, b);
    surf_node_add(parent, b->root);
    return b;
}

void surf_button_destroy(surf_button *b)
{
    if (!b)
        return;
    surf_node_destroy(b->root);
    free(b);
}

surf_node *surf_button_node(surf_button *b) { return b ? b->root : NULL; }

void surf_button_set_label(surf_button *b, const char *label)
{
    if (b)
        surf_text_set(b->label, label ? label : "");
}

void surf_button_on_press(surf_button *b, surf_change_cb cb, void *user)
{
    if (!b)
        return;
    b->cb = cb;
    b->user = user;
}
