/* Scrollview mechanics: drag with edge resistance, flick momentum, and
 * spring-back, all fixed-point and all in core so every backend feels the
 * same (DESIGN.md §2.6). Ticks assume the ~60 Hz surf_tick cadence. */
#include "surf_internal.h"

#define FRICTION_NUM   15   /* vel *= 15/16 per tick ≈ 0.94 */
#define FRICTION_DEN   16
#define MIN_VEL        (SURF_ONE / 5)   /* 0.2 px/tick: close enough to stop */
#define SPRING_SHIFT   2                /* off += (target-off)/4 per tick */
#define SPRING_SNAP    (SURF_ONE / 4)

static bool is_sv(const surf_node *n)
{
    return n && n->type == SURF_NODE_SCROLLVIEW;
}

surf_point surf_scrollview_content_size(surf_node *sv)
{
    if (!is_sv(sv))
        return (surf_point){0, 0};
    surf_rect b = {0, 0, 0, 0};
    for (surf_node *c = sv->first; c; c = c->next)
        b = surf_rect_union(b, surf_node_subtree_bounds(c, 0, 0));
    return (surf_point){(int16_t)(b.x + b.w), (int16_t)(b.y + b.h)};
}

static int32_t max_off_q16(surf_node *sv, bool y_axis)
{
    surf_point cs = surf_scrollview_content_size(sv);
    int32_t m = (y_axis ? cs.y - sv->h : cs.x - sv->w);
    return m > 0 ? m << 16 : 0;
}

bool surf_scroll_can_x(surf_node *sv) { return max_off_q16(sv, false) > 0; }
bool surf_scroll_can_y(surf_node *sv) { return max_off_q16(sv, true) > 0; }

static void scroller_add(surf_node *sv)
{
    for (int i = 0; i < surf_g.nscrollers; i++)
        if (surf_g.scrollers[i] == sv)
            return;
    if (surf_g.nscrollers < (int)(sizeof surf_g.scrollers / sizeof *surf_g.scrollers))
        surf_g.scrollers[surf_g.nscrollers++] = sv;
}

void surf_scroll_forget(surf_node *sv)
{
    for (int i = 0; i < surf_g.nscrollers; i++)
        if (surf_g.scrollers[i] == sv)
            surf_g.scrollers[i] = surf_g.scrollers[--surf_g.nscrollers];
}

/* raw drag offset with resistance past the edges: half-speed overscroll */
static int32_t resist(int32_t raw, int32_t max)
{
    if (raw < 0)
        return raw / 2;
    if (raw > max)
        return max + (raw - max) / 2;
    return raw;
}

void surf_scroll_begin(surf_node *sv, const surf_touch *t)
{
    sv->u.scroll.dragging = true;
    sv->u.scroll.down_x = sv->u.scroll.last_x = t->x;
    sv->u.scroll.down_y = sv->u.scroll.last_y = t->y;
    sv->u.scroll.drag_off_x = sv->u.scroll.off_x;
    sv->u.scroll.drag_off_y = sv->u.scroll.off_y;
    /* catching a moving list stops it */
    sv->u.scroll.vel_x = sv->u.scroll.vel_y = 0;
    surf_scroll_forget(sv);
}

void surf_scroll_touch(surf_node *sv, const surf_touch *t)
{
    if (!sv->u.scroll.dragging)
        return;

    if (t->phase == SURF_TOUCH_MOVE) {
        int32_t old_x = sv->u.scroll.off_x, old_y = sv->u.scroll.off_y;
        sv->u.scroll.off_x = resist(
            sv->u.scroll.drag_off_x + ((int32_t)(sv->u.scroll.down_x - t->x) << 16),
            max_off_q16(sv, false));
        sv->u.scroll.off_y = resist(
            sv->u.scroll.drag_off_y + ((int32_t)(sv->u.scroll.down_y - t->y) << 16),
            max_off_q16(sv, true));
        /* EMA velocity from per-event deltas (~one event per tick) */
        sv->u.scroll.vel_x = (sv->u.scroll.vel_x * 3 +
                              (((int32_t)(sv->u.scroll.last_x - t->x)) << 16) * 5) / 8;
        sv->u.scroll.vel_y = (sv->u.scroll.vel_y * 3 +
                              (((int32_t)(sv->u.scroll.last_y - t->y)) << 16) * 5) / 8;
        sv->u.scroll.last_x = t->x;
        sv->u.scroll.last_y = t->y;
        if (old_x != sv->u.scroll.off_x || old_y != sv->u.scroll.off_y)
            surf_damage_subtree(sv);
        return;
    }

    if (t->phase == SURF_TOUCH_UP) {
        sv->u.scroll.dragging = false;
        bool out_x = sv->u.scroll.off_x < 0 ||
                     sv->u.scroll.off_x > max_off_q16(sv, false);
        bool out_y = sv->u.scroll.off_y < 0 ||
                     sv->u.scroll.off_y > max_off_q16(sv, true);
        if (out_x) sv->u.scroll.vel_x = 0;
        if (out_y) sv->u.scroll.vel_y = 0;
        if (out_x || out_y ||
            sv->u.scroll.vel_x > MIN_VEL || sv->u.scroll.vel_x < -MIN_VEL ||
            sv->u.scroll.vel_y > MIN_VEL || sv->u.scroll.vel_y < -MIN_VEL)
            scroller_add(sv);
        else
            sv->u.scroll.vel_x = sv->u.scroll.vel_y = 0;
    }
}

/* one axis of momentum/spring; returns true while still moving */
static bool axis_tick(int32_t *off, int32_t *vel, int32_t max)
{
    if (*off < 0 || *off > max) {
        int32_t target = *off < 0 ? 0 : max;
        *vel = 0;
        *off += (target - *off) >> SPRING_SHIFT;
        if (*off - target < SPRING_SNAP && target - *off < SPRING_SNAP) {
            *off = target;
            return false;
        }
        return true;
    }
    if (*vel == 0)
        return false;
    *off += *vel;
    if (*off < 0 || *off > max) {
        *vel = 0;  /* crossed the edge: spring takes over next tick */
        return true;
    }
    *vel = *vel * FRICTION_NUM / FRICTION_DEN;
    if (*vel < MIN_VEL && *vel > -MIN_VEL) {
        *vel = 0;
        *off = (*off + SURF_ONE / 2) & ~(int32_t)0xffff;  /* settle on a pixel */
        return false;
    }
    return true;
}

void surf_scroll_tick(void)
{
    for (int i = 0; i < surf_g.nscrollers;) {
        surf_node *sv = surf_g.scrollers[i];
        bool ax = axis_tick(&sv->u.scroll.off_x, &sv->u.scroll.vel_x,
                            max_off_q16(sv, false));
        bool ay = axis_tick(&sv->u.scroll.off_y, &sv->u.scroll.vel_y,
                            max_off_q16(sv, true));
        surf_damage_subtree(sv);
        if (!ax && !ay)
            surf_scroll_forget(sv);  /* swaps the tail into slot i */
        else
            i++;
    }
}

/* ---- public API ---- */

surf_node *surf_scrollview_new(int16_t x, int16_t y, int16_t w, int16_t h)
{
    surf_node *n = surf_node_alloc(SURF_NODE_SCROLLVIEW);
    if (n) {
        n->x = x; n->y = y; n->w = w; n->h = h;
    }
    return n;
}

void surf_scrollview_set_offset(surf_node *sv, int16_t x, int16_t y)
{
    if (!is_sv(sv))
        return;
    int32_t nx = (int32_t)x << 16, ny = (int32_t)y << 16;
    int32_t mx = max_off_q16(sv, false), my = max_off_q16(sv, true);
    if (nx < 0) nx = 0;
    if (nx > mx) nx = mx;
    if (ny < 0) ny = 0;
    if (ny > my) ny = my;
    if (nx == sv->u.scroll.off_x && ny == sv->u.scroll.off_y)
        return;
    sv->u.scroll.off_x = nx;
    sv->u.scroll.off_y = ny;
    surf_damage_subtree(sv);
}

surf_point surf_scrollview_offset(const surf_node *sv)
{
    if (!is_sv(sv))
        return (surf_point){0, 0};
    return (surf_point){(int16_t)(sv->u.scroll.off_x >> 16),
                        (int16_t)(sv->u.scroll.off_y >> 16)};
}
