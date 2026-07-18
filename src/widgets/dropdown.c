/* Dropdown: a 9-patch box showing the selected item; tapping it attaches
 * a popup (scrim + panel + item rows) to the screen root, so it overlays
 * everything — the detach/reattach primitive doing widget work. Item rows
 * are sized handler-groups; the scrim is a screen-sized one that just
 * closes the popup. */
#include <stdlib.h>
#include <string.h>

#include "surfer.h"

#define PAD 6

struct surf_dropdown {
    surf_dropdown_style style;
    surf_node   *root, *label, *arrow;
    surf_node   *popup, *scrim, *rows, *hi;
    char       **items;
    int32_t      nitems, selected;
    int16_t      w, item_h;
    surf_index_cb cb;
    void         *user;
};

static char *dup_str(const char *s)
{
    size_t n = strlen(s) + 1;
    char *p = malloc(n);
    if (p)
        memcpy(p, s, n);
    return p;
}

static void close_popup(surf_dropdown *d)
{
    if (!d->popup)
        return;
    surf_node_destroy(d->popup);
    surf_node_destroy(d->scrim);
    d->popup = d->scrim = d->rows = d->hi = NULL;
    if (d->arrow)
        surf_filmstrip_set_frame(d->arrow, 0);
}

static void select_item(surf_dropdown *d, int32_t idx, bool fire)
{
    if (idx < 0 || idx >= d->nitems)
        return;
    bool changed = idx != d->selected;
    d->selected = idx;
    surf_text_set(d->label, d->items[idx]);
    if (fire && changed && d->cb)
        d->cb(idx, d->user);
}

static void item_touch(surf_node *n, const surf_touch *t, void *user)
{
    surf_dropdown *d = user;
    int16_t ry, ny;
    surf_node_abs_pos(d->rows, NULL, &ry);
    surf_node_abs_pos(n, NULL, &ny);
    int32_t idx = (ny - ry) / d->item_h;
    if (t->phase == SURF_TOUCH_DOWN && d->hi)
        surf_node_set_pos(d->hi, PAD, (int16_t)(idx * d->item_h));
    if (t->phase != SURF_TOUCH_UP)
        return;
    select_item(d, idx, true);
    close_popup(d);
}

static void scrim_touch(surf_node *n, const surf_touch *t, void *user)
{
    (void)n;
    if (t->phase == SURF_TOUCH_DOWN)
        close_popup((surf_dropdown *)user);
}

static void open_popup(surf_dropdown *d)
{
    int16_t ax, ay;
    surf_node_abs_pos(d->root, &ax, &ay);
    int16_t ph = (int16_t)(d->nitems * d->item_h + 2 * PAD);
    int16_t py = (int16_t)(ay + d->item_h + 2);

    d->scrim = surf_group_new(0, 0);
    surf_group_set_clip(d->scrim, INT16_MAX, INT16_MAX);
    surf_node_set_on_touch(d->scrim, scrim_touch, d);

    d->popup = surf_group_new(ax, py);
    surf_node_add(d->popup,
                  surf_ninepatch_new(d->style.panel, 0, 0, d->w, ph,
                                     d->style.inset, d->style.inset,
                                     d->style.inset, d->style.inset));
    surf_node *rows = d->rows = surf_group_new(0, PAD);
    d->hi = surf_rect_new(PAD, (int16_t)(d->selected * d->item_h),
                          (int16_t)(d->w - 2 * PAD), d->item_h, d->style.hi_color);
    surf_node_add(rows, d->hi);
    for (int32_t i = 0; i < d->nitems; i++) {
        surf_node *row = surf_group_new(0, (int16_t)(i * d->item_h));
        surf_group_set_clip(row, d->w, d->item_h);
        surf_node_set_on_touch(row, item_touch, d);
        surf_node *lbl = surf_text_new(d->style.font, d->items[i], PAD + 2, PAD / 2,
                                       d->style.text_color);
        surf_text_set_wrap(lbl, (int16_t)(d->w - 2 * PAD - 4));
        surf_text_set_ellipsis(lbl, true);
        surf_node_add(row, lbl);
        surf_node_add(rows, row);
    }
    surf_node_add(d->popup, rows);

    surf_node_add(surf_screen(), d->scrim);
    surf_node_add(surf_screen(), d->popup);
    if (d->arrow)
        surf_filmstrip_set_frame(d->arrow, 1);
}

static void box_touch(surf_node *n, const surf_touch *t, void *user)
{
    (void)n;
    surf_dropdown *d = user;
    if (t->phase != SURF_TOUCH_UP)
        return;
    if (d->popup)
        close_popup(d);
    else
        open_popup(d);
}

surf_dropdown *surf_dropdown_new(surf_node *parent, int16_t x, int16_t y, int16_t w,
                                 const surf_dropdown_style *style,
                                 const char *const *items, int32_t nitems)
{
    if (!parent || !style || !style->panel || !style->font || !items || nitems <= 0)
        return NULL;
    surf_dropdown *d = calloc(1, sizeof *d);
    if (!d)
        return NULL;
    d->style = *style;
    d->w = w;
    d->nitems = nitems;
    d->item_h = (int16_t)(surf_font_line_h(style->font) + PAD);
    d->items = calloc((size_t)nitems, sizeof *d->items);
    for (int32_t i = 0; i < nitems; i++)
        d->items[i] = dup_str(items[i]);

    d->root = surf_group_new(x, y);
    surf_node *box = surf_ninepatch_new(style->panel, 0, 0, w, d->item_h,
                                        style->inset, style->inset, style->inset,
                                        style->inset);
    d->label = surf_text_new(style->font, items[0], PAD + 2, PAD / 2,
                             style->text_color);
    if (!d->root || !box || !d->label) {
        surf_node_destroy(d->root);
        surf_node_destroy(box);
        surf_node_destroy(d->label);
        for (int32_t i = 0; i < nitems; i++)
            free(d->items[i]);
        free(d->items);
        free(d);
        return NULL;
    }
    surf_text_set_wrap(d->label, (int16_t)(w - 2 * PAD - style->arrow_w - 4));
    surf_text_set_ellipsis(d->label, true);
    surf_node_add(d->root, box);
    surf_node_add(d->root, d->label);
    if (style->arrow) {
        d->arrow = surf_filmstrip_new(style->arrow, style->arrow_w, style->arrow_h,
                                      (int16_t)(w - style->arrow_w - PAD),
                                      (int16_t)((d->item_h - style->arrow_h) / 2));
        surf_node_add(d->root, d->arrow);
    }
    surf_node_set_on_touch(d->root, box_touch, d);
    surf_node_add(parent, d->root);
    return d;
}

void surf_dropdown_destroy(surf_dropdown *d)
{
    if (!d)
        return;
    close_popup(d);
    surf_node_destroy(d->root);  /* box/label/arrow go with the subtree */
    for (int32_t i = 0; i < d->nitems; i++)
        free(d->items[i]);
    free(d->items);
    free(d);
}

surf_node *surf_dropdown_node(surf_dropdown *d) { return d ? d->root : NULL; }

int32_t surf_dropdown_selected(const surf_dropdown *d) { return d ? d->selected : 0; }

void surf_dropdown_set_selected(surf_dropdown *d, int32_t index)
{
    if (d)
        select_item(d, index, false);
}

void surf_dropdown_on_change(surf_dropdown *d, surf_index_cb cb, void *user)
{
    if (!d)
        return;
    d->cb = cb;
    d->user = user;
}
