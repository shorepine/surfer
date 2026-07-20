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
    surf_sprite_set_xform(s, SURF_ONE * 2, 0, 0);
    OK(surf_node_size(s).x == 128 && surf_node_size(s).y == 128);
    surf_sprite_set_xform(s, SURF_ONE / 2, 1, 0);
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
    surf_sprite_set_xform(s, SURF_ONE * 2, 0, 0);   /* -> 128x64 at 20,20 */
    surf_tick();
    bool covered_new = false;
    for (int i = 0; i < nops; i++)
        if (ops[i].op == 'X' && rect_eq(ops[i].r, (surf_rect){20, 20, 128, 64}))
            covered_new = true;
    OK(covered_new);

    /* clamping: absurd scales stay in the PPA's range */
    surf_sprite_set_xform(s, SURF_ONE * 100, 0, 0);
    OK(surf_sprite_scale(s) == SURF_ONE * 16);

    /* mirror alone takes the xform path, footprint unchanged */
    surf_sprite_set_xform(s, SURF_ONE, 0, 1);
    OK(surf_sprite_mirror(s) == 1);
    OK(surf_node_size(s).x == 64 && surf_node_size(s).y == 32);
    surf_tick();
    nops = 0;
    surf_node_set_pos(s, 24, 24);
    surf_tick();
    bool saw_mirror = false;
    for (int i = 0; i < nops; i++)
        if (ops[i].op == 'X' && ops[i].mirror == 1 && ops[i].rot == 0)
            saw_mirror = true;
    OK(saw_mirror);

    surf_node_destroy(s);

    /* footprint collision: overlap, separation, transform, hidden */
    surf_node *c1 = surf_sprite_new(&img64, 10, 10);
    surf_node *c2 = surf_sprite_new(&img64, 50, 50);
    surf_node_add(surf_screen(), c1);
    surf_node_add(surf_screen(), c2);
    OK(surf_node_overlaps(c1, c2));                 /* 64x64 at 40px apart */
    surf_node_set_pos(c2, 100, 10);
    OK(!surf_node_overlaps(c1, c2));
    surf_sprite_set_xform(c2, SURF_ONE * 2, 0, 0);  /* 128 wide: reaches back */
    surf_node_set_pos(c2, 70, 10);
    OK(surf_node_overlaps(c1, c2));
    surf_node_set_hidden(c2, true);
    OK(!surf_node_overlaps(c1, c2));
    surf_node_destroy(c1);
    surf_node_destroy(c2);

    /* ---- load-time image builder ---- */
    surf_image *base = surf_image_new(64, 32, SURF_FMT_RGB565);
    OK(base && base->opaque && base->stride % 64 == 0);
    surf_image_fill(base, (surf_rect){0, 0, 64, 32}, SURF_RGB(255, 0, 0));
    uint16_t *row0 = (uint16_t *)base->pixels;
    OK(row0[0] == SURF_RGB(255, 0, 0));

    surf_image *ov = surf_image_new(8, 8, SURF_FMT_ARGB8888);
    OK(ov && !ov->opaque);
    /* half-transparent green square */
    for (int y = 0; y < 8; y++)
        for (int x = 0; x < 8; x++)
            *((uint32_t *)((uint8_t *)ov->pixels + y * ov->stride) + x) =
                0x8000ff00u;
    surf_image_blit(base, ov, (surf_rect){0, 0, 8, 8}, 2, 2);
    uint16_t px = *((uint16_t *)((uint8_t *)base->pixels + 4 * base->stride) + 4);
    /* red half-blended with green: both channels present */
    OK(((px >> 11) & 0x1f) > 8 && ((px >> 5) & 0x3f) > 16);
    /* outside the blit untouched */
    OK(row0[0] == SURF_RGB(255, 0, 0));
    /* clipping doesn't crash or write out of bounds */
    surf_image_blit(base, ov, (surf_rect){0, 0, 8, 8}, -4, 28);
    surf_image_blit(base, ov, (surf_rect){0, 0, 8, 8}, 62, -2);
    surf_image_destroy(ov);
    surf_image_destroy(base);
}
