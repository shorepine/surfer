/* Textgrid: fast fixed-width text (terminals, code editors). Cells are
 * opaque (bg + tinted glyph), so painting is pure writes — no framebuffer
 * reads — done by the CPU directly into the compose target via the hal's
 * fb_ptr. Rationale, measured at M2/M5: the PPA costs ~85µs per op no
 * matter how small, so a 2,500-glyph screen is ~210ms of blits; the CPU
 * writes the same screen in ~1.2MB ≈ 15ms. This file is the sanctioned
 * exception to the no-per-pixel-in-core rule (DESIGN.md §5.6); if a hal
 * has no fb_ptr it falls back to the slow per-glyph path, still correct. */
#include <stdlib.h>
#include <string.h>

#include "surf_internal.h"

static bool is_grid(const surf_node *n)
{
    return n && n->type == SURF_NODE_TEXTGRID;
}

static surf_textcell *cell(surf_node *n, int16_t col, int16_t row)
{
    return &n->u.grid.cells[row * n->u.grid.cols + col];
}

surf_node *surf_textgrid_new(const surf_font *f, int16_t cols, int16_t rows,
                             surf_color fg, surf_color bg)
{
    if (!f || cols <= 0 || rows <= 0)
        return NULL;
    const surf_glyph *m = surf_font_glyph(f, 'M');
    if (!m)
        return NULL;
    surf_node *n = surf_node_alloc(SURF_NODE_TEXTGRID);
    if (!n)
        return NULL;
    n->u.grid.cells = calloc((size_t)cols * rows, sizeof(surf_textcell));
    if (!n->u.grid.cells) {
        surf_node_destroy(n);
        return NULL;
    }
    n->u.grid.font = f;
    n->u.grid.cols = cols;
    n->u.grid.rows = rows;
    n->u.grid.cell_w = m->adv;
    n->u.grid.cell_h = surf_font_line_h(f);
    n->u.grid.fg = fg;
    n->u.grid.bg = bg;
    n->w = (int16_t)(cols * m->adv);
    n->h = (int16_t)(rows * n->u.grid.cell_h);
    for (int32_t i = 0; i < (int32_t)cols * rows; i++)
        n->u.grid.cells[i] = (surf_textcell){' ', fg, bg};
    return n;
}

surf_point surf_textgrid_cell_size(const surf_node *n)
{
    if (!is_grid(n))
        return (surf_point){0, 0};
    return (surf_point){n->u.grid.cell_w, n->u.grid.cell_h};
}

static void damage_cells(surf_node *n, int16_t col, int16_t row,
                         int16_t ncols, int16_t nrows)
{
    /* fold the cell rect into node w/h temporarily via a child-less
     * damage: compute the sub-rect in node space and push it through the
     * same ancestor translation surf_damage_subtree uses */
    int16_t ow = n->w, oh = n->h, ox = n->x, oy = n->y;
    n->x = (int16_t)(ox + col * n->u.grid.cell_w);
    n->y = (int16_t)(oy + row * n->u.grid.cell_h);
    n->w = (int16_t)(ncols * n->u.grid.cell_w);
    n->h = (int16_t)(nrows * n->u.grid.cell_h);
    surf_damage_subtree(n);
    n->x = ox; n->y = oy; n->w = ow; n->h = oh;
}

void surf_textgrid_set_cell(surf_node *n, int16_t col, int16_t row, uint32_t cp,
                            surf_color fg, surf_color bg)
{
    if (!is_grid(n) || col < 0 || col >= n->u.grid.cols || row < 0 ||
        row >= n->u.grid.rows)
        return;
    surf_textcell *c = cell(n, col, row);
    if (c->cp == cp && c->fg == fg && c->bg == bg)
        return;
    *c = (surf_textcell){cp, fg, bg};
    damage_cells(n, col, row, 1, 1);
}

void surf_textgrid_set_row(surf_node *n, int16_t row, const char *utf8)
{
    if (!is_grid(n) || row < 0 || row >= n->u.grid.rows)
        return;
    const char *s = utf8 ? utf8 : "";
    int32_t i = 0;
    int16_t col = 0, lo = -1, hi = -1;
    while (col < n->u.grid.cols) {
        uint32_t cp = surf_utf8_next(s, &i);
        if (cp == 0 || cp == '\n')
            break;
        surf_textcell *c = cell(n, col, row);
        if (c->cp != cp || c->fg != n->u.grid.fg || c->bg != n->u.grid.bg) {
            *c = (surf_textcell){cp, n->u.grid.fg, n->u.grid.bg};
            if (lo < 0) lo = col;
            hi = col;
        }
        col++;
    }
    for (; col < n->u.grid.cols; col++) {
        surf_textcell *c = cell(n, col, row);
        if (c->cp != ' ' || c->bg != n->u.grid.bg) {
            *c = (surf_textcell){' ', n->u.grid.fg, n->u.grid.bg};
            if (lo < 0) lo = col;
            hi = col;
        }
    }
    if (lo >= 0)
        damage_cells(n, lo, row, (int16_t)(hi - lo + 1), 1);
}

void surf_textgrid_set_fast_scroll(surf_node *n, bool on)
{
    if (is_grid(n))
        n->u.grid.fast = on;
}

/* Fast path: the hal shifts the pixels; only the exposed rows need a
 * repaint, so a line-scroll costs one DMA copy + one row of cells
 * instead of a full-grid re-render (DESIGN.md §5.6). */
static bool grid_shift_pixels(surf_node *n, int16_t dy_rows, int16_t ady)
{
    if (!n->u.grid.fast || !surf_g.hal->scroll_rect ||
        ady >= n->u.grid.rows || !surf_node_attached(n) ||
        (n->flags & SURF_NF_HIDDEN))
        return false;
    int16_t ax, ay;
    surf_node_abs_pos(n, &ax, &ay);
    surf_rect r = surf_rect_intersect(
        (surf_rect){ax, ay, n->w, n->h},
        (surf_rect){0, 0, surf_g.w, surf_g.h});
    if (r.w != n->w || r.h != n->h)
        return false;  /* partially off-screen: take the slow path */
    surf_g.hal->scroll_rect(r, (int16_t)(dy_rows * n->u.grid.cell_h));
    return true;
}

void surf_textgrid_scroll(surf_node *n, int16_t dy_rows)
{
    if (!is_grid(n) || dy_rows == 0)
        return;
    int16_t rows = n->u.grid.rows, cols = n->u.grid.cols;
    int16_t ady = dy_rows < 0 ? (int16_t)-dy_rows : dy_rows;
    bool shifted = grid_shift_pixels(n, dy_rows, ady);
    if (ady >= rows) {
        for (int32_t i = 0; i < (int32_t)cols * rows; i++)
            n->u.grid.cells[i] = (surf_textcell){' ', n->u.grid.fg, n->u.grid.bg};
    } else if (dy_rows > 0) {  /* content moves up */
        memmove(cell(n, 0, 0), cell(n, 0, dy_rows),
                (size_t)cols * (rows - ady) * sizeof(surf_textcell));
        for (int32_t i = (int32_t)cols * (rows - ady); i < (int32_t)cols * rows; i++)
            n->u.grid.cells[i] = (surf_textcell){' ', n->u.grid.fg, n->u.grid.bg};
    } else {
        memmove(cell(n, 0, ady), cell(n, 0, 0),
                (size_t)cols * (rows - ady) * sizeof(surf_textcell));
        for (int32_t i = 0; i < (int32_t)cols * ady; i++)
            n->u.grid.cells[i] = (surf_textcell){' ', n->u.grid.fg, n->u.grid.bg};
    }
    if (shifted) {
        /* the hal moved the surviving pixels; repaint only the exposure */
        damage_cells(n, 0, dy_rows > 0 ? (int16_t)(rows - ady) : 0, cols, ady);
    } else {
        surf_damage_subtree(n);
    }
}

/* ---- paint ---- */

static inline uint16_t mix565(surf_color fgc, surf_color bgc, uint32_t a)
{
    if (a >= 255) return fgc;
    if (a == 0) return bgc;
    uint32_t fr = (fgc >> 11) & 0x1f, fg_ = (fgc >> 5) & 0x3f, fb = fgc & 0x1f;
    uint32_t br = (bgc >> 11) & 0x1f, bg_ = (bgc >> 5) & 0x3f, bb = bgc & 0x1f;
    uint32_t r = (fr * a + br * (255 - a)) / 255;
    uint32_t g = (fg_ * a + bg_ * (255 - a)) / 255;
    uint32_t b = (fb * a + bb * (255 - a)) / 255;
    return (uint16_t)((r << 11) | (g << 5) | b);
}

/* one cell into the framebuffer, clipped to vis (screen coords) */
static void cell_to_fb(const surf_node *n, const surf_textcell *c,
                       int16_t cx, int16_t cy, surf_rect vis,
                       uint8_t *fb, int32_t stride)
{
    const surf_font *f = n->u.grid.font;
    surf_rect box = {cx, cy, n->u.grid.cell_w, n->u.grid.cell_h};
    surf_rect v = surf_rect_intersect(box, vis);
    if (surf_rect_empty(v))
        return;

    const surf_glyph *g = (c->cp == ' ') ? NULL : surf_font_glyph(f, c->cp);
    int16_t gx = 0, gy = 0, gx1 = 0, gy1 = 0;
    const uint8_t *atlas = NULL;
    int32_t astride = 0;
    if (g && g->w > 0) {
        gx = (int16_t)(cx + g->xoff);
        gy = (int16_t)(cy + f->ascent + g->yoff);
        gx1 = (int16_t)(gx + g->w);
        gy1 = (int16_t)(gy + g->h);
        atlas = (const uint8_t *)f->atlas.pixels;
        astride = f->atlas.stride;
    }

    for (int16_t y = v.y; y < v.y + v.h; y++) {
        uint16_t *row = (uint16_t *)(fb + (int32_t)y * stride) + v.x;
        if (atlas && y >= gy && y < gy1) {
            const uint8_t *arow = atlas + (int32_t)(g->y + (y - gy)) * astride + g->x;
            for (int16_t x = v.x; x < v.x + v.w; x++, row++) {
                if (x >= gx && x < gx1)
                    *row = mix565(c->fg, c->bg, arow[x - gx]);
                else
                    *row = c->bg;
            }
        } else {
            for (int16_t x = 0; x < v.w; x++)
                row[x] = c->bg;
        }
    }
}

void surf_textgrid_paint(const surf_paint_ent *e)
{
    surf_node *n = e->n;
    int16_t cw = n->u.grid.cell_w, ch = n->u.grid.cell_h;

    int16_t c0 = (int16_t)((e->vis.x - e->ax) / cw);
    int16_t r0 = (int16_t)((e->vis.y - e->ay) / ch);
    int16_t c1 = (int16_t)((e->vis.x + e->vis.w - 1 - e->ax) / cw);
    int16_t r1 = (int16_t)((e->vis.y + e->vis.h - 1 - e->ay) / ch);
    if (c1 >= n->u.grid.cols) c1 = (int16_t)(n->u.grid.cols - 1);
    if (r1 >= n->u.grid.rows) r1 = (int16_t)(n->u.grid.rows - 1);

    int32_t stride = 0;
    uint8_t *fb = surf_g.hal->fb_ptr ? surf_g.hal->fb_ptr(&stride) : NULL;

    for (int16_t r = r0; r <= r1; r++) {
        for (int16_t c = c0; c <= c1; c++) {
            const surf_textcell *tc = cell(n, c, r);
            int16_t cx = (int16_t)(e->ax + c * cw), cy = (int16_t)(e->ay + r * ch);
            if (fb) {
                cell_to_fb(n, tc, cx, cy, e->vis, fb, stride);
                continue;
            }
            /* fallback for hals without fb_ptr: fill + glyph blend */
            surf_rect box = surf_rect_intersect((surf_rect){cx, cy, cw, ch}, e->vis);
            if (surf_rect_empty(box))
                continue;
            surf_g.hal->fill(box, tc->bg);
            const surf_glyph *g = tc->cp == ' ' ? NULL
                                                : surf_font_glyph(n->u.grid.font, tc->cp);
            if (g && g->w > 0) {
                surf_image img = n->u.grid.font->atlas;
                img.tint = tc->fg;
                surf_glyph_blit(&img, g, (int16_t)(cx + g->xoff),
                                (int16_t)(cy + n->u.grid.font->ascent + g->yoff),
                                e->vis);
            }
        }
    }
}
