/* Textinput node: single-line editable text with caret + selection.
 * Editing happens at event time (allocation allowed); painting is glyph
 * blits + two fills, same frame path as everything else. The box art and
 * on-screen keyboard are widgets built on top (DESIGN.md §2.5). */
#include <stdlib.h>
#include <string.h>

#include "surf_internal.h"

#define CARET_W 2
#define PAD 2  /* caret breathing room at the box edges */

static bool is_input(const surf_node *n)
{
    return n && n->type == SURF_NODE_TEXTINPUT;
}

/* pen x of the caret at byte index `idx` (unscrolled) */
static int16_t caret_x(const surf_node *n, int32_t idx)
{
    const surf_font *f = n->u.input.font;
    const char *s = n->u.input.buf ? n->u.input.buf : "";
    int16_t x = 0;
    int32_t i = 0;
    uint32_t prev = 0;
    while (i < idx) {
        uint32_t cp = surf_utf8_next(s, &i);
        if (cp == 0)
            break;
        const surf_glyph *g = surf_font_glyph(f, cp);
        if (g)
            x = (int16_t)(x + g->adv + surf_font_kern(f, prev, cp));
        prev = cp;
    }
    return x;
}

static void scroll_into_view(surf_node *n)
{
    int16_t cx = caret_x(n, n->u.input.caret);
    int16_t view = (int16_t)(n->w - CARET_W - PAD);
    if (cx - n->u.input.scroll_x > view)
        n->u.input.scroll_x = (int16_t)(cx - view);
    if (cx - n->u.input.scroll_x < 0)
        n->u.input.scroll_x = cx;
    if (n->u.input.scroll_x < 0)
        n->u.input.scroll_x = 0;
}

static void input_changed(surf_node *n)
{
    scroll_into_view(n);
    surf_damage_subtree(n);  /* box-sized: one small rect */
}

surf_node *surf_textinput_new(const surf_font *f, int16_t x, int16_t y,
                              int16_t w, surf_color c)
{
    if (!f || w <= 0)
        return NULL;
    surf_node *n = surf_node_alloc(SURF_NODE_TEXTINPUT);
    if (!n)
        return NULL;
    n->x = x;
    n->y = y;
    n->w = w;
    n->h = surf_font_line_h(f);
    n->u.input.font = f;
    n->u.input.img = f->atlas;
    n->u.input.img.tint = c;
    n->flags |= SURF_NF_GRAB;  /* drag-select is never a scroll */
    return n;
}

void surf_textinput_set_text(surf_node *n, const char *str)
{
    if (!is_input(n))
        return;
    int32_t len = (int32_t)strlen(str ? str : "");
    char *buf = malloc((size_t)len + 1);
    if (!buf)
        return;
    memcpy(buf, str ? str : "", (size_t)len + 1);
    free(n->u.input.buf);
    n->u.input.buf = buf;
    n->u.input.len = len;
    n->u.input.cap = len + 1;
    n->u.input.caret = n->u.input.anchor = len;
    n->u.input.scroll_x = 0;
    input_changed(n);
}

const char *surf_textinput_text(const surf_node *n)
{
    return is_input(n) && n->u.input.buf ? n->u.input.buf : "";
}

static void delete_range(surf_node *n, int32_t a, int32_t b)
{
    if (a > b) { int32_t t = a; a = b; b = t; }
    memmove(n->u.input.buf + a, n->u.input.buf + b, (size_t)(n->u.input.len - b + 1));
    n->u.input.len -= b - a;
    n->u.input.caret = n->u.input.anchor = a;
}

void surf_textinput_insert(surf_node *n, const char *utf8)
{
    if (!is_input(n) || !utf8 || !utf8[0])
        return;
    if (!n->u.input.buf)
        surf_textinput_set_text(n, "");
    if (n->u.input.caret != n->u.input.anchor)
        delete_range(n, n->u.input.anchor, n->u.input.caret);

    int32_t add = (int32_t)strlen(utf8);
    if (n->u.input.len + add + 1 > n->u.input.cap) {
        int32_t cap = (n->u.input.len + add + 1) * 2;
        char *nb = realloc(n->u.input.buf, (size_t)cap);
        if (!nb)
            return;
        n->u.input.buf = nb;
        n->u.input.cap = cap;
    }
    int32_t at = n->u.input.caret;
    memmove(n->u.input.buf + at + add, n->u.input.buf + at,
            (size_t)(n->u.input.len - at + 1));
    memcpy(n->u.input.buf + at, utf8, (size_t)add);
    n->u.input.len += add;
    n->u.input.caret = n->u.input.anchor = at + add;
    input_changed(n);
}

void surf_textinput_backspace(surf_node *n)
{
    if (!is_input(n) || !n->u.input.buf)
        return;
    if (n->u.input.caret != n->u.input.anchor)
        delete_range(n, n->u.input.anchor, n->u.input.caret);
    else if (n->u.input.caret > 0)
        delete_range(n, surf_utf8_prev(n->u.input.buf, n->u.input.caret),
                     n->u.input.caret);
    else
        return;
    input_changed(n);
}

void surf_textinput_delete(surf_node *n)
{
    if (!is_input(n) || !n->u.input.buf)
        return;
    if (n->u.input.caret != n->u.input.anchor) {
        delete_range(n, n->u.input.anchor, n->u.input.caret);
    } else if (n->u.input.caret < n->u.input.len) {
        int32_t j = n->u.input.caret;
        surf_utf8_next(n->u.input.buf, &j);
        delete_range(n, n->u.input.caret, j);
    } else {
        return;
    }
    input_changed(n);
}

int32_t surf_textinput_caret(const surf_node *n)
{
    return is_input(n) ? n->u.input.caret : 0;
}

void surf_textinput_set_caret(surf_node *n, int32_t byte_idx, bool extend)
{
    if (!is_input(n))
        return;
    if (byte_idx < 0) byte_idx = 0;
    if (byte_idx > n->u.input.len) byte_idx = n->u.input.len;
    n->u.input.caret = byte_idx;
    if (!extend)
        n->u.input.anchor = byte_idx;
    input_changed(n);
}

void surf_textinput_move(surf_node *n, int32_t delta_cp, bool extend)
{
    if (!is_input(n) || !n->u.input.buf)
        return;
    int32_t i = n->u.input.caret;
    while (delta_cp > 0 && i < n->u.input.len) {
        surf_utf8_next(n->u.input.buf, &i);
        delta_cp--;
    }
    while (delta_cp < 0 && i > 0) {
        i = surf_utf8_prev(n->u.input.buf, i);
        delta_cp++;
    }
    surf_textinput_set_caret(n, i, extend);
}

int32_t surf_textinput_index_from_x(const surf_node *n, int16_t local_x)
{
    if (!is_input(n) || !n->u.input.buf)
        return 0;
    const surf_font *f = n->u.input.font;
    const char *s = n->u.input.buf;
    int32_t x = local_x + n->u.input.scroll_x;
    int16_t pen = 0;
    int32_t i = 0;
    uint32_t prev = 0;
    for (;;) {
        int32_t j = i;
        uint32_t cp = surf_utf8_next(s, &j);
        if (cp == 0)
            return i;
        const surf_glyph *g = surf_font_glyph(f, cp);
        int16_t adv = g ? (int16_t)(g->adv + surf_font_kern(f, prev, cp)) : 0;
        if (x < pen + adv / 2)
            return i;
        pen = (int16_t)(pen + adv);
        prev = cp;
        i = j;
    }
}

void surf_textinput_set_focused(surf_node *n, bool focused)
{
    if (!is_input(n) || focused == !!(n->flags & SURF_NF_FOCUS))
        return;
    if (focused)
        n->flags |= SURF_NF_FOCUS;
    else
        n->flags &= (uint8_t)~SURF_NF_FOCUS;
    surf_damage_subtree(n);
}

void surf_textinput_paint(const surf_paint_ent *e)
{
    surf_node *n = e->n;
    const surf_font *f = n->u.input.font;
    const char *s = n->u.input.buf ? n->u.input.buf : "";
    int16_t sx = n->u.input.scroll_x;

    /* selection highlight behind the glyphs */
    int32_t sa = n->u.input.anchor, sb = n->u.input.caret;
    if (sa > sb) { int32_t t = sa; sa = sb; sb = t; }
    if (sa != sb) {
        int16_t x0 = (int16_t)(caret_x(n, sa) - sx), x1 = (int16_t)(caret_x(n, sb) - sx);
        surf_rect sel = {(int16_t)(e->ax + x0), e->ay, (int16_t)(x1 - x0), n->h};
        sel = surf_rect_intersect(sel, e->vis);
        if (!surf_rect_empty(sel))
            surf_g.hal->fill(sel, SURF_RGB(60, 90, 140));
    }

    surf_tlayout it;
    surf_tglyph tg;
    surf_tlayout_begin(&it, f, s, 0, SURF_ALIGN_LEFT, 0);
    while (surf_tlayout_next(&it, &tg)) {
        if (tg.g->w <= 0)
            continue;
        surf_glyph_blit(&n->u.input.img, tg.g,
                        (int16_t)(e->ax + tg.x - sx), (int16_t)(e->ay + tg.y), e->vis);
    }

    if (n->flags & SURF_NF_FOCUS) {
        int16_t cx = (int16_t)(caret_x(n, n->u.input.caret) - sx);
        surf_rect caret = {(int16_t)(e->ax + cx), e->ay, CARET_W, n->h};
        caret = surf_rect_intersect(caret, e->vis);
        if (!surf_rect_empty(caret))
            surf_g.hal->fill(caret, n->u.input.img.tint);
    }
}
