/* Textgrid tests: cell model, row fill, scroll, damage granularity, and
 * the CPU fast path writing real pixels into the mock framebuffer. Uses
 * the synthetic font from test_text.c (cell = 'M' advance 10 × line 16). */
#include <string.h>

#include "mock_hal.h"

extern surf_font tfont;

static uint16_t px(int x, int y)
{
    return mock_fb[y * mock_w + x];
}

static void test_grid_model(void)
{
    fresh(400, 200, 32);

    surf_node *g = surf_textgrid_new(&tfont, 10, 4, 0xffff, 0x0000);
    OK(g && g->w == 100 && g->h == 64);
    OK(surf_textgrid_cell_size(g).x == 10 && surf_textgrid_cell_size(g).y == 16);
    surf_node_add(surf_screen(), g);
    surf_tick();

    /* row fill + damage bounded to the touched cells */
    surf_textgrid_set_row(g, 1, "AB");
    OK(surf_g.dirty.n == 1);
    /* cells 0..1 changed; the space-pad right of them was already spaces */
    OK(rect_eq(surf_g.dirty.r[0], (surf_rect){0, 16, 20, 16}));
    surf_tick();

    /* single cell damage */
    surf_textgrid_set_cell(g, 5, 2, 'Z', 1, 2);
    OK(surf_g.dirty.n == 1);
    OK(rect_eq(surf_g.dirty.r[0], (surf_rect){50, 32, 10, 16}));
    surf_tick();

    /* unchanged writes are free */
    surf_textgrid_set_cell(g, 5, 2, 'Z', 1, 2);
    OK(surf_g.dirty.n == 0);

    /* scroll up by one row moves content and blanks the last row */
    surf_textgrid_set_row(g, 0, "QQ");
    surf_tick();
    surf_textgrid_scroll(g, 1);
    OK(surf_g.dirty.n == 1);
    OK(rect_eq(surf_g.dirty.r[0], (surf_rect){0, 0, 100, 64}));
    surf_tick();
    /* row0 now holds former row1 ("AB"), row1 former row2 (Z at col 5) */
    surf_textgrid_set_row(g, 0, "AB");  /* identical content → no damage */
    OK(surf_g.dirty.n == 0);
    surf_textgrid_set_cell(g, 5, 1, 'Z', 1, 2);
    OK(surf_g.dirty.n == 0);

    surf_node_destroy(g);
}

static void test_grid_pixels(void)
{
    fresh(400, 200, 32);

    /* grid at (20, 10); 'A' glyph in tfont: cell 10x16, glyph 8x10 at
     * xoff 1, yoff -10 from the 12px ascent → glyph box y 2..12, x 1..9 */
    surf_node *g = surf_textgrid_new(&tfont, 4, 2, 0xffff, 0x1234);
    surf_node_add(surf_screen(), g);
    surf_node_set_pos(g, 20, 10);
    surf_tick();

    /* bg everywhere in an empty cell */
    OK(px(21, 11) == 0x1234);
    OK(px(20 + 39, 10 + 31) == 0x1234);

    surf_textgrid_set_cell(g, 1, 0, 'A', 0xffff, 0x1234);
    surf_tick();
    /* the synthetic atlas pixels are fake memory, so glyph coverage values
     * are arbitrary — but the cell must still be fully written: corners of
     * the cell outside the glyph box are exactly bg */
    OK(px(20 + 10, 10 + 0) == 0x1234);   /* cell top-left, above glyph */
    OK(px(20 + 19, 10 + 15) == 0x1234);  /* cell bottom-right, below glyph */

    /* opaque: a textgrid covering the dirty rect suppresses the bg fill */
    nops = 0;
    surf_textgrid_set_cell(g, 2, 1, 'B', 0xffff, 0x1234);
    surf_tick();
    for (int i = 0; i < nops; i++)
        OK(ops[i].op != 'F' || ops[i].c != SURF_RGB(0, 0, 0));

    surf_node_destroy(g);
}

void run_grid_tests(void)
{
    test_grid_model();
    test_grid_pixels();
}
