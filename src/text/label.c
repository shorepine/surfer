/* Label node: a string drawn as A8 atlas blits. Layout happens during
 * paint via the shared walker — the cost is the blits themselves, and a
 * damaged label repaints only its dirty intersection. */
#include <stdlib.h>
#include <string.h>

#include "surf_internal.h"

/* strdup is POSIX, not C11 */
static char *dup_str(const char *s)
{
    size_t n = strlen(s) + 1;
    char *p = malloc(n);
    if (p)
        memcpy(p, s, n);
    return p;
}

/* node w/h drive damage and hit test; keep them true to the layout */
static void text_update_bounds(surf_node *n)
{
    const surf_font *f = n->u.text.font;
    if ((n->u.text.tflags & SURF_TF_ELLIPSIS) && n->u.text.wrap_w > 0) {
        n->w = n->u.text.wrap_w;
        n->h = surf_font_line_h(f);
        return;
    }
    surf_point sz = surf_text_measure(f, n->u.text.str, n->u.text.wrap_w);
    n->w = (n->u.text.wrap_w > 0) ? n->u.text.wrap_w : sz.x;
    n->h = sz.y;
}

surf_node *surf_text_new(const surf_font *f, const char *str,
                         int16_t x, int16_t y, surf_color c)
{
    if (!f)
        return NULL;
    surf_node *n = surf_node_alloc(SURF_NODE_TEXT);
    if (!n)
        return NULL;
    n->x = x;
    n->y = y;
    n->u.text.font = f;
    n->u.text.str = str ? dup_str(str) : NULL;
    n->u.text.img = f->atlas;
    n->u.text.img.tint = c;
    text_update_bounds(n);
    return n;
}

void surf_text_set(surf_node *n, const char *str)
{
    if (!n || n->type != SURF_NODE_TEXT)
        return;
    surf_damage_subtree(n);
    free(n->u.text.str);
    n->u.text.str = str ? dup_str(str) : NULL;
    text_update_bounds(n);
    surf_damage_subtree(n);
}

void surf_text_set_color(surf_node *n, surf_color c)
{
    if (!n || n->type != SURF_NODE_TEXT || n->u.text.img.tint == c)
        return;
    n->u.text.img.tint = c;
    surf_damage_subtree(n);
}

void surf_text_set_wrap(surf_node *n, int16_t wrap_w)
{
    if (!n || n->type != SURF_NODE_TEXT || n->u.text.wrap_w == wrap_w)
        return;
    surf_damage_subtree(n);
    n->u.text.wrap_w = wrap_w;
    text_update_bounds(n);
    surf_damage_subtree(n);
}

void surf_text_set_align(surf_node *n, surf_align a)
{
    if (!n || n->type != SURF_NODE_TEXT || n->u.text.align == (uint8_t)a)
        return;
    n->u.text.align = (uint8_t)a;
    surf_damage_subtree(n);
}

void surf_text_set_ellipsis(surf_node *n, bool on)
{
    if (!n || n->type != SURF_NODE_TEXT)
        return;
    uint8_t tf = on ? (uint8_t)(n->u.text.tflags | SURF_TF_ELLIPSIS)
                    : (uint8_t)(n->u.text.tflags & ~SURF_TF_ELLIPSIS);
    if (tf == n->u.text.tflags)
        return;
    surf_damage_subtree(n);
    n->u.text.tflags = tf;
    text_update_bounds(n);
    surf_damage_subtree(n);
}

/* shared by label and textinput paint: one glyph, clipped to vis */
void surf_glyph_blit(const surf_image *img, const surf_glyph *g,
                     int16_t dx, int16_t dy, surf_rect vis)
{
    surf_rect dst = {dx, dy, g->w, g->h};
    surf_rect v = surf_rect_intersect(dst, vis);
    if (surf_rect_empty(v))
        return;
    surf_rect src = {
        (int16_t)(g->x + (v.x - dx)), (int16_t)(g->y + (v.y - dy)), v.w, v.h,
    };
    surf_g.hal->blend(img, src, (surf_point){v.x, v.y}, 255);
}

void surf_text_paint(const surf_paint_ent *e)
{
    surf_node *n = e->n;
    if (!n->u.text.str)
        return;
    surf_tlayout it;
    surf_tglyph tg;
    surf_tlayout_begin(&it, n->u.text.font, n->u.text.str, n->u.text.wrap_w,
                       n->u.text.align, n->u.text.tflags);
    while (surf_tlayout_next(&it, &tg)) {
        if (tg.g->w <= 0)
            continue;  /* spaces advance the pen, nothing to blit */
        surf_glyph_blit(&n->u.text.img, tg.g,
                        (int16_t)(e->ax + tg.x), (int16_t)(e->ay + tg.y), e->vis);
    }
}

void surf_text_free_storage(surf_node *n)
{
    if (n->type == SURF_NODE_TEXT) {
        free(n->u.text.str);
        n->u.text.str = NULL;
    } else if (n->type == SURF_NODE_TEXTINPUT) {
        free(n->u.input.buf);
        n->u.input.buf = NULL;
    }
}
