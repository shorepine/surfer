/* Atlas text engine: UTF-8, glyph/kern lookup, greedy word wrap with
 * per-glyph advance tables (DESIGN.md §2.5). One layout walker feeds
 * measure, paint, and caret math so they can never disagree. */
#include "surf_internal.h"

#define ELLIPSIS_CP 0x2026u

uint32_t surf_utf8_next(const char *s, int32_t *i)
{
    const uint8_t *p = (const uint8_t *)s + *i;
    uint8_t b = p[0];
    if (b == 0)
        return 0;
    if (b < 0x80) { *i += 1; return b; }
    if ((b & 0xe0) == 0xc0 && (p[1] & 0xc0) == 0x80) {
        *i += 2;
        return ((uint32_t)(b & 0x1f) << 6) | (p[1] & 0x3f);
    }
    if ((b & 0xf0) == 0xe0 && (p[1] & 0xc0) == 0x80 && (p[2] & 0xc0) == 0x80) {
        *i += 3;
        return ((uint32_t)(b & 0x0f) << 12) | ((uint32_t)(p[1] & 0x3f) << 6) |
               (p[2] & 0x3f);
    }
    if ((b & 0xf8) == 0xf0 && (p[1] & 0xc0) == 0x80 && (p[2] & 0xc0) == 0x80 &&
        (p[3] & 0xc0) == 0x80) {
        *i += 4;
        return ((uint32_t)(b & 0x07) << 18) | ((uint32_t)(p[1] & 0x3f) << 12) |
               ((uint32_t)(p[2] & 0x3f) << 6) | (p[3] & 0x3f);
    }
    *i += 1;  /* invalid byte: emit replacement-ish, keep moving */
    return 0xfffd;
}

uint32_t surf_utf8_first(const char *s)
{
    if (!s || !s[0])
        return 0;
    int32_t i = 0;
    return surf_utf8_next(s, &i);
}

int32_t surf_utf8_prev(const char *s, int32_t i)
{
    if (i <= 0)
        return 0;
    i--;
    while (i > 0 && ((uint8_t)s[i] & 0xc0) == 0x80)
        i--;
    return i;
}

const surf_glyph *surf_font_glyph(const surf_font *f, uint32_t cp)
{
    int32_t lo = 0, hi = f->nglyphs - 1;
    while (lo <= hi) {
        int32_t mid = (lo + hi) / 2;
        if (f->glyphs[mid].cp == cp)
            return &f->glyphs[mid];
        if (f->glyphs[mid].cp < cp)
            lo = mid + 1;
        else
            hi = mid - 1;
    }
    if (cp != '?')
        return surf_font_glyph(f, '?');
    return NULL;
}

int16_t surf_font_kern(const surf_font *f, uint32_t a, uint32_t b)
{
    if (a == 0 || f->nkerns == 0)
        return 0;
    int32_t lo = 0, hi = f->nkerns - 1;
    while (lo <= hi) {
        int32_t mid = (lo + hi) / 2;
        const surf_kern *k = &f->kerns[mid];
        if (k->a == a && k->b == b)
            return k->adv;
        if (k->a < a || (k->a == a && k->b < b))
            lo = mid + 1;
        else
            hi = mid - 1;
    }
    return 0;
}

static int16_t adv_of(const surf_font *f, uint32_t prev, uint32_t cp)
{
    const surf_glyph *g = surf_font_glyph(f, cp);
    return g ? (int16_t)(g->adv + surf_font_kern(f, prev, cp)) : 0;
}

/* Greedy line breaker: returns the render end of the line starting at
 * `start`, its width, and where the next line begins (-1 if none).
 * Breaks after a space (space excluded from width) or after a hyphen
 * (included); a word wider than wrap_w hard-breaks mid-word. */
static void next_line(const surf_font *f, const char *s, int32_t start,
                      int16_t wrap_w, int32_t *end, int16_t *width,
                      int32_t *next)
{
    int32_t i = start, brk = -1;
    int16_t w = 0, brk_w = 0;
    uint32_t prev = 0;

    for (;;) {
        int32_t j = i;
        uint32_t cp = surf_utf8_next(s, &j);
        if (cp == 0) {
            *end = i; *width = w; *next = -1;
            return;
        }
        if (cp == '\n') {
            *end = i; *width = w; *next = j;
            return;
        }
        int16_t adv = adv_of(f, prev, cp);
        if (wrap_w > 0 && w + adv > wrap_w && i > start) {
            if (brk >= 0) {
                *end = brk; *width = brk_w; *next = brk;
            } else {
                *end = i; *width = w; *next = i;  /* hard break */
            }
            /* soft-broken lines don't start with the break's spaces */
            int32_t k = *next;
            for (;;) {
                int32_t k2 = k;
                if (surf_utf8_next(s, &k2) != ' ')
                    break;
                k = k2;
            }
            *next = k;
            return;
        }
        w = (int16_t)(w + adv);
        if (cp == ' ') { brk = j; brk_w = (int16_t)(w - adv); }
        if (cp == '-') { brk = j; brk_w = w; }
        prev = cp;
        i = j;
    }
}

static int16_t indent(int16_t wrap_w, int16_t line_w, uint8_t align)
{
    if (wrap_w <= 0 || line_w >= wrap_w)
        return 0;
    return (int16_t)((wrap_w - line_w) * align / 2);
}

/* Enter a line: set pen, and in ellipsize mode pre-truncate it. */
static void enter_line(surf_tlayout *it, int32_t start)
{
    int16_t line_w;
    next_line(it->f, it->s, start,
              (it->tflags & SURF_TF_ELLIPSIS) ? 0 : it->wrap_w,
              &it->line_end, &line_w, &it->next_start);
    it->i = start;
    it->prev_cp = 0;
    it->ell = 0;

    if ((it->tflags & SURF_TF_ELLIPSIS) && it->wrap_w > 0 && line_w > it->wrap_w) {
        const surf_glyph *e = surf_font_glyph(it->f, ELLIPSIS_CP);
        int16_t ew = e ? e->adv : 0;
        int32_t i = start, keep = start;
        int16_t w = 0;
        uint32_t prev = 0;
        for (;;) {
            int32_t j = i;
            uint32_t cp = surf_utf8_next(it->s, &j);
            if (cp == 0 || cp == '\n' || j > it->line_end)
                break;
            int16_t adv = adv_of(it->f, prev, cp);
            if (w + adv + ew > it->wrap_w)
                break;
            w = (int16_t)(w + adv);
            prev = cp;
            keep = j;
            i = j;
        }
        it->line_end = keep;
        it->next_start = -1;  /* ellipsize renders a single line */
        it->ell = 1;
        line_w = (int16_t)(w + ew);
    }
    it->pen_x = indent(it->wrap_w, line_w, it->align);
}

void surf_tlayout_begin(surf_tlayout *it, const surf_font *f, const char *s,
                        int16_t wrap_w, uint8_t align, uint8_t tflags)
{
    it->f = f;
    it->s = s ? s : "";
    it->wrap_w = wrap_w;
    it->align = align;
    it->tflags = tflags;
    it->base_y = f->ascent;
    enter_line(it, 0);
}

bool surf_tlayout_next(surf_tlayout *it, surf_tglyph *out)
{
    for (;;) {
        if (it->i >= it->line_end) {
            if (it->ell == 1) {
                const surf_glyph *e = surf_font_glyph(it->f, ELLIPSIS_CP);
                it->ell = 2;
                if (e) {
                    out->g = e;
                    out->x = (int16_t)(it->pen_x + e->xoff);
                    out->y = (int16_t)(it->base_y + e->yoff);
                    out->byte_idx = it->line_end;
                    it->pen_x = (int16_t)(it->pen_x + e->adv);
                    return true;
                }
            }
            if (it->next_start < 0)
                return false;
            it->base_y = (int16_t)(it->base_y + surf_font_line_h(it->f));
            enter_line(it, it->next_start);
            continue;
        }
        int32_t idx = it->i;
        uint32_t cp = surf_utf8_next(it->s, &it->i);
        const surf_glyph *g = surf_font_glyph(it->f, cp);
        if (!g)
            continue;
        it->pen_x = (int16_t)(it->pen_x + surf_font_kern(it->f, it->prev_cp, cp));
        out->g = g;
        out->x = (int16_t)(it->pen_x + g->xoff);
        out->y = (int16_t)(it->base_y + g->yoff);
        out->byte_idx = idx;
        it->pen_x = (int16_t)(it->pen_x + g->adv);
        it->prev_cp = cp;
        return true;
    }
}

surf_point surf_text_measure(const surf_font *f, const char *str, int16_t wrap_w)
{
    if (!f)
        return (surf_point){0, 0};
    const char *s = str ? str : "";
    int16_t max_w = 0, lines = 1;
    int32_t start = 0;
    for (;;) {
        int32_t end, next;
        int16_t w;
        next_line(f, s, start, wrap_w, &end, &w, &next);
        if (w > max_w)
            max_w = w;
        if (next < 0)
            break;
        lines++;
        start = next;
    }
    return (surf_point){max_w, (int16_t)(lines * surf_font_line_h(f))};
}
