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

/* screen row -> ring row. Without scrollback head/view are 0 and
 * total_rows == rows, so this is the identity it always was. */
static int16_t ring_row(const surf_node *n, int16_t row)
{
    int16_t t = n->u.grid.total_rows;
    int16_t r = (int16_t)((n->u.grid.head + row - n->u.grid.view) % t);
    return r < 0 ? (int16_t)(r + t) : r;
}

static surf_textcell *cell(surf_node *n, int16_t col, int16_t row)
{
    return &n->u.grid.cells[ring_row(n, row) * n->u.grid.cols + col];
}

/* Writing anything, or scrolling, means the user is doing something live:
 * snap the view back to the bottom the way a terminal does. Returns true
 * when the view actually moved, so the caller can repaint the lot. */
static bool snap_live(surf_node *n)
{
    if (n->u.grid.view == 0)
        return false;
    n->u.grid.view = 0;
    surf_damage_subtree(n);
    return true;
}

static void blank_ring_row(surf_node *n, int16_t rr)
{
    surf_textcell *row = &n->u.grid.cells[rr * n->u.grid.cols];
    for (int16_t c = 0; c < n->u.grid.cols; c++)
        row[c] = (surf_textcell){' ', n->u.grid.fg, n->u.grid.bg};
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
    n->u.grid.total_rows = rows;   /* set_scrollback grows this */
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

/* Recolour the whole grid. Every cell carries its own pair, so the
 * defaults alone would only reach cells written from here on — the
 * point of this is a console changing its background while you watch,
 * so it rewrites the ones that still hold the OLD default and leaves
 * anything a caller deliberately coloured. */
void surf_textgrid_set_colors(surf_node *n, surf_color fg, surf_color bg)
{
    if (!n || n->type != SURF_NODE_TEXTGRID)
        return;
    surf_color ofg = n->u.grid.fg, obg = n->u.grid.bg;
    if (ofg == fg && obg == bg)
        return;
    n->u.grid.fg = fg;
    n->u.grid.bg = bg;
    int32_t cells = (int32_t)n->u.grid.cols * n->u.grid.total_rows;
    for (int32_t i = 0; i < cells; i++) {
        surf_textcell *c = &n->u.grid.cells[i];
        if (c->fg == ofg)
            c->fg = fg;
        if (c->bg == obg)
            c->bg = bg;
    }
    surf_node_damage(n);
}

surf_point surf_textgrid_cell_size(const surf_node *n)
{
    if (!is_grid(n))
        return (surf_point){0, 0};
    return (surf_point){n->u.grid.cell_w, n->u.grid.cell_h};
}

/* True if this cell's glyph is wider than one cell — see cell_glyph()
 * below, which is the same question asked at paint time. Declared here
 * because DAMAGE has to ask it too: a wide glyph is painted by its left
 * cell across two, so damaging only that cell leaves the right half
 * un-repainted. Setting one cell to an emoji then drew half of it, and
 * the other half appeared later when something unrelated damaged the
 * neighbour — which is the worst shape of bug, since it looks like a
 * rendering glitch rather than a damage one. */
static bool cell_is_wide(surf_node *n, int16_t col, int16_t row);

static void damage_cells(surf_node *n, int16_t col, int16_t row,
                         int16_t ncols, int16_t nrows)
{
    /* Extend one cell right if the last column damaged holds a wide
     * glyph. The LEFT side needs no equivalent here — the paint loop
     * starts a cell early for its own reasons and finds it. */
    for (int16_t r = row; r < row + nrows; r++) {
        if (col + ncols <= n->u.grid.cols &&
            cell_is_wide(n, (int16_t)(col + ncols - 1), r)) {
            ncols++;
            break;
        }
    }
    if (col + ncols > n->u.grid.cols)
        ncols = (int16_t)(n->u.grid.cols - col);

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

/* set_cell/set_row snap to live first: text arriving while you are
 * scrolled back would otherwise land off-screen, invisibly. */
void surf_textgrid_set_cell(surf_node *n, int16_t col, int16_t row, uint32_t cp,
                            surf_color fg, surf_color bg)
{
    if (is_grid(n))
        snap_live(n);
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
    if (is_grid(n))
        snap_live(n);
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
        /* A WIDE GLYPH TAKES THE NEXT CELL TOO, so the rest of the
         * string starts one further along. Without this the character
         * after an emoji lands in a cell the emoji is drawn over, and
         * the line reads as though that character had been eaten —
         * "fire" then a space then "cpu" comes out "firecpu". The cell
         * is blanked rather than left alone because it is what the row
         * would show if the emoji were later replaced by something
         * narrow.
         *
         * This is set_row's business and not set_cells'. Here the caller
         * hands over a whole line and tracks no columns; a caller using
         * set_cells is doing its own column arithmetic (tulip2's console
         * and its vt shadow both do) and a hidden extra advance would
         * desync it. */
        if (col < n->u.grid.cols && cell_is_wide(n, (int16_t)(col - 1), row)) {
            surf_textcell *nx = cell(n, col, row);
            if (nx->cp != ' ' || nx->fg != n->u.grid.fg ||
                nx->bg != n->u.grid.bg) {
                *nx = (surf_textcell){' ', n->u.grid.fg, n->u.grid.bg};
                if (lo < 0) lo = col;
                hi = col;
            }
            col++;
        }
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
        surf_node_effectively_hidden(n))
        return false;
    int16_t ax, ay;
    surf_node_abs_pos(n, &ax, &ay);
    surf_rect r = surf_rect_intersect(
        (surf_rect){ax, ay, n->w, n->h},
        (surf_rect){0, 0, surf_g.w, surf_g.h});
    if (r.w != n->w || r.h != n->h)
        return false;  /* partially off-screen: take the slow path */
    int16_t px = (int16_t)(ady * n->u.grid.cell_h);
    surf_g.hal->scroll_rect(r, (int16_t)(dy_rows * n->u.grid.cell_h));
    /* the console's own scrollbar sits over the grid: same smear */
    surf_damage_above(n, r, 0, px);
    return true;
}

void surf_textgrid_scroll(surf_node *n, int16_t dy_rows)
{
    if (!is_grid(n) || dy_rows == 0)
        return;
    int16_t rows = n->u.grid.rows, cols = n->u.grid.cols;
    int16_t ady = dy_rows < 0 ? (int16_t)-dy_rows : dy_rows;
    bool snapped = snap_live(n);
    bool shifted = !snapped && grid_shift_pixels(n, dy_rows, ady);

    if (n->u.grid.total_rows > rows) {
        /* Scrollback: don't move any cells, move the window. The rows
         * that leave the top stay in the ring and become history — which
         * is the whole point — and the cost is O(exposed rows) instead of
         * O(screen). */
        int16_t t = n->u.grid.total_rows;
        if (ady >= rows) {
            for (int16_t i = 0; i < rows; i++)
                blank_ring_row(n, ring_row(n, i));
            n->u.grid.hist = 0;
        } else if (dy_rows > 0) {
            for (int16_t i = 0; i < ady; i++) {
                n->u.grid.head = (int16_t)((n->u.grid.head + 1) % t);
                blank_ring_row(n, ring_row(n, (int16_t)(rows - 1)));
                if (n->u.grid.hist < t - rows)
                    n->u.grid.hist++;
            }
        } else {
            for (int16_t i = 0; i < ady; i++) {
                n->u.grid.head = (int16_t)((n->u.grid.head - 1 + t) % t);
                blank_ring_row(n, ring_row(n, 0));
                if (n->u.grid.hist > 0)
                    n->u.grid.hist--;
            }
        }
        if (shifted)
            damage_cells(n, 0, dy_rows > 0 ? (int16_t)(rows - ady) : 0, cols, ady);
        else
            surf_damage_subtree(n);
        return;
    }

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

/* ---- scrollback ---- */

/* Drag anywhere on the grid scrolls the view. The visible bar is a
 * separate surf_scrollbar the caller places and keeps in step (poll
 * surf_textgrid_view); this handler exists so the TEXT is draggable too,
 * which is what a touchscreen wants. */
static void grid_touch(surf_node *n, const surf_touch *t, void *user)
{
    (void)user;
    if (n->u.grid.hist <= 0)
        return;
    int16_t ax, ay;
    surf_node_abs_pos(n, &ax, &ay);
    int16_t ly = (int16_t)(t->y - ay);
    if (t->phase == SURF_TOUCH_DOWN) {
        n->u.grid.drag_from = n->u.grid.view;
        n->u.grid.drag_y = ly;
        return;
    }
    if (t->phase != SURF_TOUCH_MOVE)
        return;
    /* dragging DOWN reveals older lines, like flicking a page down */
    int32_t dy = ly - n->u.grid.drag_y;
    int32_t rows = dy / (n->u.grid.cell_h ? n->u.grid.cell_h : 1);
    int32_t v = n->u.grid.drag_from + rows;
    if (v < 0)
        v = 0;
    if (v > n->u.grid.hist)
        v = n->u.grid.hist;
    if (v != n->u.grid.view) {
        n->u.grid.view = (int16_t)v;
        surf_damage_subtree(n);
    }
}

bool surf_textgrid_set_scrollback(surf_node *n, int16_t mult)
{
    if (!is_grid(n) || mult < 1)
        return false;
    int32_t total = (int32_t)n->u.grid.rows * mult;
    if (total > 32000)                 /* int16 row indices */
        return false;
    surf_textcell *cells = calloc((size_t)n->u.grid.cols * total,
                                  sizeof(surf_textcell));
    if (!cells)
        return false;
    /* keep what is on screen: it lands at ring rows 0..rows-1 */
    memcpy(cells, n->u.grid.cells,
           (size_t)n->u.grid.cols * n->u.grid.rows * sizeof(surf_textcell));
    for (int32_t i = (int32_t)n->u.grid.cols * n->u.grid.rows;
         i < (int32_t)n->u.grid.cols * total; i++)
        cells[i] = (surf_textcell){' ', n->u.grid.fg, n->u.grid.bg};
    free(n->u.grid.cells);
    n->u.grid.cells = cells;
    n->u.grid.total_rows = (int16_t)total;
    n->u.grid.head = n->u.grid.hist = n->u.grid.view = 0;
    surf_node_set_on_touch(n, grid_touch, NULL);
    return true;
}

int16_t surf_textgrid_history(const surf_node *n)
{
    return is_grid(n) ? n->u.grid.hist : 0;
}

int16_t surf_textgrid_view(const surf_node *n)
{
    return is_grid(n) ? n->u.grid.view : 0;
}

void surf_textgrid_set_view(surf_node *n, int16_t back)
{
    if (!is_grid(n))
        return;
    if (back < 0)
        back = 0;
    if (back > n->u.grid.hist)
        back = n->u.grid.hist;
    if (back != n->u.grid.view) {
        n->u.grid.view = back;
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

/* A cell's glyph, and which face it came from.
 *
 * `wide` is the one bit of policy here: a glyph whose advance does not
 * fit a cell owns the NEXT cell too. That is the East-Asian-Wide rule
 * every terminal follows, and for a grid it is not a convention but an
 * arithmetic necessity — an emoji is square and a mono cell is not, so
 * "as tall as the line" and "one cell wide" cannot both hold. mono16 is
 * 10x19 and two of its cells are 20x19, which is as square as a cell
 * grid gets. */
typedef struct {
    const surf_glyph *g;
    const surf_font  *f;
    bool              wide;
} cellglyph;

static bool cell_is_wide(surf_node *n, int16_t col, int16_t row)
{
    if (col < 0 || col >= n->u.grid.cols || row < 0 || row >= n->u.grid.rows)
        return false;
    const surf_textcell *c = cell(n, col, row);
    if (c->cp == ' ')
        return false;
    const surf_font *src;
    const surf_glyph *g = surf_font_glyph_in(n->u.grid.font, c->cp, &src);
    return g && g->adv > n->u.grid.cell_w;
}

static cellglyph cell_glyph(const surf_node *n, const surf_textcell *c)
{
    cellglyph r = {NULL, n->u.grid.font, false};
    if (c->cp == ' ')
        return r;
    r.g = surf_font_glyph_in(n->u.grid.font, c->cp, &r.f);
    if (r.g && r.g->adv > n->u.grid.cell_w)
        r.wide = true;
    return r;
}

/* one cell into the framebuffer, clipped to vis (screen coords).
 * box_w is the cell's own width, or two cells' for a wide glyph. */
static void cell_to_fb(const surf_node *n, const surf_textcell *c,
                       cellglyph cg, int16_t cx, int16_t cy, int16_t box_w,
                       surf_rect vis, uint8_t *fb, int32_t stride)
{
    const surf_font *f = n->u.grid.font;
    surf_rect box = {cx, cy, box_w, n->u.grid.cell_h};
    surf_rect v = surf_rect_intersect(box, vis);
    if (surf_rect_empty(v))
        return;

    const surf_font *gf = cg.f;
    const surf_glyph *g = cg.g;
    int16_t gx = 0, gy = 0, gx1 = 0, gy1 = 0;
    const uint8_t *atlas = NULL;
    int32_t astride = 0;
    bool color = false;
    if (g && g->w > 0) {
        /* Centre the glyph's own advance box in whatever box it owns. For
         * the grid's own face those are equal (a mono cell IS the advance)
         * and this is the old `cx + xoff`; it only does anything for a
         * fallback glyph in a two-cell box. */
        int16_t pen = (int16_t)(cx + (box_w - g->adv) / 2);
        gx = (int16_t)(pen + g->xoff);
        if (gf != f) {
            /* A FALLBACK glyph does not share this face's baseline — it
             * was baked against its own — so in a grid it is centred in
             * the cell box instead. A cell is a box and an emoji is a
             * picture that belongs in it; borrowing a baseline from a
             * face it has never met puts a 16px picture 1px above a 19px
             * cell and clips its top row. (A LABEL centres in the LINE
             * box for the same reason — glyph_top() in font.c; it used
             * to baseline, which clipped every emoji's top by
             * size − ascent.) Emoji are square, so the advance is also
             * the box height. */
            gy = (int16_t)(cy + (n->u.grid.cell_h - g->adv) / 2
                              + g->adv + g->yoff);
        } else {
            gy = (int16_t)(cy + f->ascent + g->yoff);
        }
        gx1 = (int16_t)(gx + g->w);
        gy1 = (int16_t)(gy + g->h);
        atlas = (const uint8_t *)gf->atlas.pixels;
        astride = gf->atlas.stride;
        color = gf->atlas.format == SURF_FMT_ARGB8888;
    }

    for (int16_t y = v.y; y < v.y + v.h; y++) {
        uint16_t *row = (uint16_t *)(fb + (int32_t)y * stride) + v.x;
        if (atlas && y >= gy && y < gy1) {
            if (color) {
                /* An emoji carries its own colours, so the cell's fg means
                 * nothing to it — only the bg, which it is composited over.
                 * Straight (un-premultiplied) ARGB, matching what the hal's
                 * blend and surf_image_blit read. */
                const uint32_t *arow =
                    (const uint32_t *)(atlas +
                        (int32_t)(g->y + (y - gy)) * astride) + g->x;
                for (int16_t x = v.x; x < v.x + v.w; x++, row++) {
                    if (x < gx || x >= gx1) {
                        *row = c->bg;
                        continue;
                    }
                    uint32_t p = arow[x - gx];
                    uint32_t a = p >> 24;
                    if (a == 0) {
                        *row = c->bg;
                    } else {
                        uint16_t fg = SURF_RGB((p >> 16) & 0xff,
                                               (p >> 8) & 0xff, p & 0xff);
                        *row = a >= 255 ? fg : mix565(fg, c->bg, a);
                    }
                }
            } else {
                const uint8_t *arow =
                    atlas + (int32_t)(g->y + (y - gy)) * astride + g->x;
                for (int16_t x = v.x; x < v.x + v.w; x++, row++) {
                    if (x >= gx && x < gx1)
                        *row = mix565(c->fg, c->bg, arow[x - gx]);
                    else
                        *row = c->bg;
                }
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

    /* START ONE CELL EARLY. A wide glyph is painted by its LEFT cell, so
     * a damage rect that begins on its right half would otherwise find a
     * cell owned by somebody off-screen and paint nothing there — the
     * right half of every emoji on a partial repaint. Its own damage
     * intersect makes the extra cell nearly free. */
    int16_t cstart = c0 > 0 ? (int16_t)(c0 - 1) : c0;

    for (int16_t r = r0; r <= r1; r++) {
        bool owned = false;     /* this cell belongs to the wide one left of it */
        for (int16_t c = cstart; c <= c1; c++) {
            const surf_textcell *tc = cell(n, c, r);
            if (owned) {
                owned = false;
                continue;
            }
            cellglyph cg = cell_glyph(n, tc);
            int16_t span = (cg.wide && c + 1 < n->u.grid.cols) ? 2 : 1;
            owned = span > 1;
            if (c < c0 && span == 1)
                continue;       /* only walked to find a wide left neighbour */
            int16_t cx = (int16_t)(e->ax + c * cw), cy = (int16_t)(e->ay + r * ch);
            if (fb) {
                cell_to_fb(n, tc, cg, cx, cy, (int16_t)(span * cw),
                           e->vis, fb, stride);
                continue;
            }
            /* fallback for hals without fb_ptr: fill + glyph blend */
            surf_rect box = surf_rect_intersect(
                (surf_rect){cx, cy, (int16_t)(span * cw), ch}, e->vis);
            if (surf_rect_empty(box))
                continue;
            surf_g.hal->fill(box, tc->bg);
            if (cg.g && cg.g->w > 0) {
                /* whichever face answered — its own atlas, tinted only if
                 * it is a mask (an emoji brings its own colours) */
                surf_image img = cg.f->atlas;
                if (img.format == SURF_FMT_A8)
                    img.tint = tc->fg;
                int16_t pen = (int16_t)(cx + (span * cw - cg.g->adv) / 2);
                int16_t gy = cg.f != n->u.grid.font
                    ? (int16_t)(cy + (ch - cg.g->adv) / 2 + cg.g->adv + cg.g->yoff)
                    : (int16_t)(cy + n->u.grid.font->ascent + cg.g->yoff);
                surf_glyph_blit(&img, cg.g, (int16_t)(pen + cg.g->xoff), gy,
                                e->vis, 255);
            }
        }
    }
}
