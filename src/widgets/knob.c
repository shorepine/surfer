/* Knob: one filmstrip node. Vertical-drag relative mapping by default —
 * the DAW convention — with angular as the configurable alternative
 * (DESIGN.md §2.6). Value changes just pick a pre-rendered frame; a drag
 * repaints one small rect. */
#include <math.h>
#ifndef M_PI    /* strict C11 on glibc and MinGW hide it; macOS does not */
#define M_PI 3.14159265358979323846
#endif
#include <stdlib.h>

#include "surfer.h"
#include "widget_touch.h"

/* Pixels of vertical drag for a full value sweep. */
#define KNOB_DRAG_RANGE 200
/* A press that travelled less than this is a TAP, not a drag. */
#define KNOB_TAP_SLOP 6

/* The house grey, when a style asks for no colour of its own. */
#define KNOB_DEFAULT_COLOR SURF_RGB(150, 154, 168)

struct surf_knob {
    surf_image     img;   /* our own tint over the style's shared pixels */
    surf_node     *root, *strip;
    int16_t        fw, fh, frames;
    uint8_t        mode;
    int32_t        value;  /* Q16 */
    int32_t        drag_start_value;
    int16_t        drag_start_y;
    surf_change_cb cb;
    void          *user;
    /* A tap is reported SEPARATELY from the value, because it means
     * something else entirely: not "the value is now this" but "the user
     * pointed AT this knob", which a caller turns into a settings popup.
     * It carries WHERE in the knob's height it landed, so the policy —
     * "the top third opens the panel" — stays with the caller. A widget
     * should have no opinion about which part of itself is special. */
    surf_change_cb tap_cb;
    void          *tap_user;
    int16_t        down_x, down_y;
    int32_t        moved;
    uint8_t        busy;   /* the contact driving it; 0 = idle */
};

static int32_t clamp_q16(int32_t v)
{
    if (v < 0) return 0;
    if (v > SURF_ONE) return SURF_ONE;
    return v;
}

static void knob_apply(surf_knob *k)
{
    int32_t f = (int32_t)(((int64_t)k->value * (k->frames - 1) + SURF_ONE / 2) >> 16);
    surf_filmstrip_set_frame(k->strip, (int16_t)f);
}

static void knob_set(surf_knob *k, int32_t v)
{
    v = clamp_q16(v);
    if (v == k->value)
        return;
    k->value = v;
    knob_apply(k);
    if (k->cb)
        k->cb(v, k->user);
}

static int32_t angular_value(const surf_knob *k, int16_t x, int16_t y)
{
    int16_t ax, ay;
    surf_node_abs_pos(k->root, &ax, &ay);
    float dx = (float)(x - (ax + k->fw / 2));
    float dy = (float)(y - (ay + k->fh / 2));
    /* 0 rad = straight up; sweep is ±3π/4, matching the baked art. */
    float ang = atan2f(dx, -dy);
    const float sweep = 3.0f * (float)M_PI / 4.0f;
    if (ang < -sweep) ang = -sweep;
    if (ang > sweep) ang = sweep;
    return (int32_t)((ang + sweep) / (2.0f * sweep) * SURF_ONE);
}

static void knob_touch(surf_node *n, const surf_touch *t, void *user)
{
    (void)n;
    surf_knob *k = user;
    if (!surf_widget_claim(&k->busy, t))
        return;                    /* a second finger on the same knob */

    if (k->mode == SURF_KNOB_DRAG_ANGULAR) {
        if (t->phase != SURF_TOUCH_UP)
            knob_set(k, angular_value(k, t->x, t->y));
        return;
    }

    switch (t->phase) {
    case SURF_TOUCH_DOWN:
        k->drag_start_y = t->y;
        k->drag_start_value = k->value;
        k->down_x = t->x;
        k->down_y = t->y;
        k->moved = 0;
        break;
    case SURF_TOUCH_MOVE: {
        int32_t dx = t->x - k->down_x, dy = t->y - k->down_y;
        if (dx < 0) dx = -dx;
        if (dy < 0) dy = -dy;
        if (dx + dy > k->moved)
            k->moved = dx + dy;
        knob_set(k, k->drag_start_value +
                        (int32_t)((int64_t)(k->drag_start_y - t->y) * SURF_ONE /
                                  KNOB_DRAG_RANGE));
        break;
    }
    case SURF_TOUCH_UP:
        /* A TAP: a press that never travelled. The value is already back
         * where it started — a zero-travel drag computes the value it
         * began at — so there is nothing to undo and this is purely an
         * extra report. Decided at UP, the same rule and the same slop
         * the selector uses for its own tap. */
        if (k->moved <= KNOB_TAP_SLOP && k->tap_cb != NULL) {
            int16_t ax, ay;
            surf_node_abs_pos(k->strip, &ax, &ay);
            surf_point sz = surf_node_size(k->strip);
            int32_t frac = 0;
            if (sz.y > 0) {
                frac = (int32_t)((int64_t)(t->y - ay) * SURF_ONE / sz.y);
                if (frac < 0)
                    frac = 0;
                if (frac > SURF_ONE)
                    frac = SURF_ONE;
            }
            k->tap_cb(frac, k->tap_user);
        }
        break;
    }
}

surf_knob *surf_knob_new(surf_node *parent, int16_t x, int16_t y,
                         const surf_knob_style *style)
{
    if (!parent || !style || !style->strip || style->frames < 2)
        return NULL;

    surf_knob *k = calloc(1, sizeof *k);
    if (!k)
        return NULL;
    k->fw = style->frame_w;
    k->fh = style->frame_h;
    k->frames = style->frames;

    /* A struct COPY: the pixels stay shared, the tint is ours. That is
     * what lets one A8 filmstrip be every colour on the panel at no cost
     * — on the P4 the tint is a palette register the PPA applies during
     * the blend it was doing anyway. Same trick as led.c. */
    k->img = *style->strip;
    k->img.tint = style->color ? style->color : KNOB_DEFAULT_COLOR;

    k->root = surf_group_new(x, y);
    k->strip = surf_filmstrip_new(&k->img, style->frame_w, style->frame_h, 0, 0);
    if (!k->root || !k->strip) {
        surf_node_destroy(k->root);
        surf_node_destroy(k->strip);
        free(k);
        return NULL;
    }
    surf_node_add(k->root, k->strip);
    surf_node_set_on_touch(k->root, knob_touch, k);
    surf_node_set_gesture_grab(k->root, true);  /* a knob drag is never a scroll */
    surf_node_add(parent, k->root);
    return k;
}

void surf_knob_destroy(surf_knob *k)
{
    if (!k)
        return;
    surf_node_destroy(k->root);   /* holds &k->img: order matters */
    free(k);
}

surf_node *surf_knob_node(surf_knob *k) { return k ? k->root : NULL; }

void surf_knob_set_color(surf_knob *k, surf_color c)
{
    if (!k || k->img.tint == c)
        return;
    k->img.tint = c;
    surf_node_damage(k->strip);   /* a retint is a repaint, not a reblit */
}

surf_color surf_knob_color(const surf_knob *k) { return k ? k->img.tint : 0; }

void surf_knob_on_tap(surf_knob *k, surf_change_cb cb, void *user)
{
    if (!k)
        return;
    k->tap_cb = cb;
    k->tap_user = user;
}

void surf_knob_set_mode(surf_knob *k, surf_knob_mode mode)
{
    if (k)
        k->mode = (uint8_t)mode;
}

void surf_knob_set_value(surf_knob *k, int32_t value_q16)
{
    if (!k)
        return;
    k->value = clamp_q16(value_q16);
    knob_apply(k);
}

int32_t surf_knob_value(const surf_knob *k) { return k ? k->value : 0; }

void surf_knob_on_change(surf_knob *k, surf_change_cb cb, void *user)
{
    if (!k)
        return;
    k->cb = cb;
    k->user = user;
}
