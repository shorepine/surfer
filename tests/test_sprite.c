/* Transformed sprites: footprint math, damage on xform writes, and the
 * compose path handing transformed draws to hal->xform_blend. */
#include "mock_hal.h"

void run_sprite_tests(void);

static uint16_t px_dummy[64 * 64];
static const surf_image img64 = {
    .pixels = px_dummy, .w = 64, .h = 64, .stride = 64 * 2,
    .format = SURF_FMT_RGB565, .opaque = false,
};

void run_sprite_tests(void)
{
    fresh(200, 200, 16);
    surf_node *s = surf_sprite_new(&img64, 10, 10);
    surf_node_add(surf_screen(), s);
    surf_tick();
    nops = 0;

    /* identity transform stays on the plain blend path */
    surf_node_set_pos(s, 12, 10);
    surf_tick();
    bool sawA = false, sawX = false;
    for (int i = 0; i < nops; i++) {
        if (ops[i].op == 'A') sawA = true;
        if (ops[i].op == 'X') sawX = true;
    }
    OK(sawA && !sawX);

    /* footprint: scale 2x doubles, quarter turn swaps sides */
    surf_sprite_set_xform(s, SURF_ONE * 2, 0);
    OK(surf_node_size(s).x == 128 && surf_node_size(s).y == 128);
    surf_sprite_set_xform(s, SURF_ONE / 2, 1);
    OK(surf_node_size(s).x == 32 && surf_node_size(s).y == 32);
    surf_sprite_set_src(s, (surf_rect){0, 0, 64, 32});
    OK(surf_node_size(s).x == 16 && surf_node_size(s).y == 32);  /* rot 1: swapped */
    OK(surf_sprite_scale(s) == SURF_ONE / 2 && surf_sprite_rot(s) == 1);

    /* transformed draw goes through xform_blend with the right rects */
    surf_tick();
    nops = 0;
    surf_node_set_pos(s, 20, 20);
    surf_tick();
    sawX = false;
    for (int i = 0; i < nops; i++)
        if (ops[i].op == 'X') {
            sawX = true;
            OK(rect_eq(ops[i].r, (surf_rect){20, 20, 16, 32}));
            OK(ops[i].rot == 1);
            OK(rect_eq(ops[i].src, (surf_rect){0, 0, 64, 32}));
            /* vis stays inside the footprint */
            OK(rect_eq(surf_rect_intersect(ops[i].vis, ops[i].r), ops[i].vis));
        }
    OK(sawX);

    /* an xform write redraws the new footprint (old is inside it here) */
    surf_tick();
    nops = 0;
    surf_sprite_set_xform(s, SURF_ONE * 2, 0);   /* -> 128x64 at 20,20 */
    surf_tick();
    bool covered_new = false;
    for (int i = 0; i < nops; i++)
        if (ops[i].op == 'X' && rect_eq(ops[i].r, (surf_rect){20, 20, 128, 64}))
            covered_new = true;
    OK(covered_new);

    /* clamping: absurd scales stay in the PPA's range */
    surf_sprite_set_xform(s, SURF_ONE * 100, 0);
    OK(surf_sprite_scale(s) == SURF_ONE * 16);

    surf_node_destroy(s);
}
