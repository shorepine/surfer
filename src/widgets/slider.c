/* Vertical slider: 9-patch track + cap sprite. The cap centers on the
 * finger and the whole widget captures the pointer, so drags never lose
 * the finger (DESIGN.md §2.6). */
#include <stdlib.h>

#include "surfer.h"

struct surf_slider {
    surf_node     *root, *track, *cap;
    int16_t        w, h, cap_w, cap_h;
    int32_t        value;  /* Q16 */
    surf_change_cb cb;
    void          *user;
};

static int32_t clamp_q16(int32_t v)
{
    if (v < 0) return 0;
    if (v > SURF_ONE) return SURF_ONE;
    return v;
}

static void slider_apply(surf_slider *s)
{
    int range = s->h - s->cap_h;
    int cap_y = range - (int)(((int64_t)s->value * range) >> 16);
    surf_node_set_pos(s->cap, (int16_t)((s->w - s->cap_w) / 2), (int16_t)cap_y);
}

static void slider_touch(surf_node *n, const surf_touch *t, void *user)
{
    (void)n;
    surf_slider *s = user;
    if (t->phase == SURF_TOUCH_UP)
        return;

    int16_t ax, ay;
    surf_node_abs_pos(s->root, &ax, &ay);
    int range = s->h - s->cap_h;
    int cap_y = t->y - ay - s->cap_h / 2;
    if (cap_y < 0) cap_y = 0;
    if (cap_y > range) cap_y = range;

    int32_t v = (int32_t)(((int64_t)(range - cap_y) << 16) / range);
    if (v == s->value)
        return;
    s->value = v;
    slider_apply(s);
    if (s->cb)
        s->cb(v, s->user);
}

surf_slider *surf_slider_new(surf_node *parent, int16_t x, int16_t y,
                             int16_t w, int16_t h, const surf_slider_style *style)
{
    if (!parent || !style || !style->track || !style->cap ||
        h <= style->cap->h || w < style->cap->w)
        return NULL;

    surf_slider *s = calloc(1, sizeof *s);
    if (!s)
        return NULL;
    s->w = w;
    s->h = h;
    s->cap_w = style->cap->w;
    s->cap_h = style->cap->h;

    s->root = surf_group_new(x, y);
    /* Exact-size track art is one blit; the tiled 9-patch is the fallback
     * for sizes the theme didn't bake. On the P4 the per-op cost of tiling
     * dwarfs the pixels (M2 bench), so themes should ship exact sizes. */
    if (style->track->w == w && style->track->h == h)
        s->track = surf_sprite_new(style->track, 0, 0);
    else
        s->track = surf_ninepatch_new(style->track, 0, 0, w, h, style->inset,
                                      style->inset, style->inset, style->inset);
    s->cap = surf_sprite_new(style->cap, 0, 0);
    if (!s->root || !s->track || !s->cap) {
        surf_node_destroy(s->root);
        surf_node_destroy(s->track);
        surf_node_destroy(s->cap);
        free(s);
        return NULL;
    }
    surf_node_add(s->root, s->track);
    surf_node_add(s->root, s->cap);
    surf_node_set_on_touch(s->root, slider_touch, s);
    surf_node_set_gesture_grab(s->root, true);  /* a slider drag is never a scroll */
    slider_apply(s);
    surf_node_add(parent, s->root);
    return s;
}

void surf_slider_destroy(surf_slider *s)
{
    if (!s)
        return;
    surf_node_destroy(s->root);
    free(s);
}

surf_node *surf_slider_node(surf_slider *s) { return s ? s->root : NULL; }

void surf_slider_set_value(surf_slider *s, int32_t value_q16)
{
    if (!s)
        return;
    s->value = clamp_q16(value_q16);
    slider_apply(s);
}

int32_t surf_slider_value(const surf_slider *s) { return s ? s->value : 0; }

void surf_slider_on_change(surf_slider *s, surf_change_cb cb, void *user)
{
    if (!s)
        return;
    s->cb = cb;
    s->user = user;
}
