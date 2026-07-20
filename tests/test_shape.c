/* Load-time shape rasterizer: coverage, winding, gradients, A8 masks. */
#include "mock_hal.h"

void run_shape_tests(void);

static uint16_t px565(const surf_image *img, int x, int y)
{
    return *(const uint16_t *)((const uint8_t *)img->pixels +
                               y * img->stride + x * 2);
}

void run_shape_tests(void)
{
    fresh(64, 64, 8);

    /* filled triangle on 565: inside solid, outside untouched, edge AA */
    surf_image *im = surf_image_new(48, 48, SURF_FMT_RGB565);
    surf_paint red = {.kind = SURF_PAINT_SOLID, .c0 = 0xf800, .a0 = 255};
    int32_t tri[6] = {24 << 16, 4 << 16, 44 << 16, 44 << 16, 4 << 16, 44 << 16};
    surf_image_poly(im, tri, 3, &red);
    OK(px565(im, 24, 30) == 0xf800);          /* deep inside */
    OK(px565(im, 2, 6) == 0x0000);            /* well outside */
    uint16_t edge = px565(im, 14, 24);        /* on the slanted edge band */
    OK(edge != 0x0000);                        /* something was drawn nearby */

    /* horizontal gradient endpoints */
    surf_paint g = {.kind = SURF_PAINT_LINEAR, .c0 = 0x0000, .c1 = 0xffff,
                    .a0 = 255, .a1 = 255,
                    .x0 = 0, .y0 = 0, .x1 = 48 << 16, .y1 = 0};
    int32_t quad[8] = {0, 0, 48 << 16, 0, 48 << 16, 48 << 16, 0, 48 << 16};
    surf_image_poly(im, quad, 4, &g);
    OK(px565(im, 1, 24) < 0x2000);            /* near-black at left */
    OK(px565(im, 46, 24) > 0xe000);           /* near-white at right */

    /* thick line: on-line filled, far-off empty; overlap joins don't hole */
    surf_image *ln = surf_image_new(48, 48, SURF_FMT_RGB565);
    int32_t zig[6] = {6 << 16, 6 << 16, 24 << 16, 40 << 16, 42 << 16, 6 << 16};
    surf_image_polyline(ln, zig, 3, 6 << 16, &red);
    OK(px565(ln, 24, 38) == 0xf800);          /* the joint itself is solid */
    OK(px565(ln, 24, 6) == 0x0000);
    surf_image_destroy(ln);

    /* circle into an A8 mask: coverage in alpha, image non-opaque */
    surf_image *m = surf_image_new(40, 40, SURF_FMT_A8);
    OK(m && !m->opaque && m->format == SURF_FMT_A8 && m->tint == 0xffff);
    surf_paint half = {.kind = SURF_PAINT_SOLID, .c0 = 0xffff, .a0 = 128};
    surf_image_ellipse(m, 20 << 16, 20 << 16, 15 << 16, 15 << 16, 0, &half);
    const uint8_t *ap = m->pixels;
    OK(ap[20 * m->stride + 20] > 100 && ap[20 * m->stride + 20] < 156);
    OK(ap[2 * m->stride + 2] == 0);
    /* ...and it drives the tinted blend path like any A8 image */
    surf_node *spr = surf_sprite_new(m, 4, 4);
    surf_node_add(surf_screen(), spr);
    surf_tick();
    nops = 0;
    surf_node_damage(spr);
    surf_tick();
    bool sawA = false;
    for (int i = 0; i < nops; i++)
        if (ops[i].op == 'A') sawA = true;
    OK(sawA);
    surf_node_destroy(spr);
    surf_image_destroy(m);

    /* bezier: curve passes through neither endpoint-line nor empty space */
    surf_image *bz = surf_image_new(48, 48, SURF_FMT_RGB565);
    int32_t bpts[8] = {4 << 16, 44 << 16, 16 << 16, 4 << 16,
                       32 << 16, 4 << 16, 44 << 16, 44 << 16};
    surf_image_bezier(bz, bpts, 4 << 16, &red);
    OK(px565(bz, 24, 14) == 0xf800);          /* the arch's crown */
    OK(px565(bz, 24, 40) == 0x0000);          /* under the arch */
    surf_image_destroy(bz);
    surf_image_destroy(im);
}
