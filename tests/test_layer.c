/* Layers: wrap-segment drawing, the streaming band fast path (shift op +
 * sliver + overlay damage), and the stop-motion full repaint. */
#include "mock_hal.h"

void run_layer_tests(void);

static uint16_t strip_px[256 * 32];
static const surf_image strip256 = {
    .pixels = strip_px, .w = 256, .h = 32, .stride = 256 * 2,
    .format = SURF_FMT_RGB565, .opaque = true,
};

void run_layer_tests(void)
{
    fresh(200, 100, 16);
    surf_node *l = surf_layer_new(&strip256, 0, 10, 200);
    surf_node_add(surf_screen(), l);
    OK(surf_node_size(l).x == 200 && surf_node_size(l).y == 32);
    surf_tick();

    /* off 100: vis 200 wide needs cols 100..255 then 0..43 → 2 blits */
    nops = 0;
    surf_layer_set_offset(l, 100 << 16);  /* slow path: no fast flag */
    surf_tick();
    int blits = 0;
    surf_rect seg0 = {0, 0, 0, 0}, seg1 = {0, 0, 0, 0};
    for (int i = 0; i < nops; i++)
        if (ops[i].op == 'B') {
            if (blits == 0) seg0 = ops[i].src;
            if (blits == 1) seg1 = ops[i].src;
            blits++;
        }
    OK(blits == 2);
    OK(seg0.x == 100 && seg0.w == 156);
    OK(seg1.x == 0 && seg1.w == 44);

    /* fast path: +3px → band_shift(-3), one 3px sliver at the right */
    surf_layer_set_fast_scroll(l, true);
    surf_tick();
    nops = 0;
    surf_layer_set_offset(l, 103 << 16);
    surf_tick();
    bool saw_shift = false, saw_sliver = false;
    for (int i = 0; i < nops; i++) {
        if (ops[i].op == 'S') {
            saw_shift = true;
            OK(rect_eq(ops[i].r, (surf_rect){0, 10, 200, 32}));
            OK(ops[i].dst.x == -3 && ops[i].dst.y == 0);
        }
        if (ops[i].op == 'B' && ops[i].dst.x == 197)
            saw_sliver = true;
    }
    OK(saw_shift && saw_sliver);

    /* sub-pixel move after a shift: one full-band repaint (stop rule) */
    nops = 0;
    surf_layer_set_offset(l, (103 << 16) + 100);
    surf_tick();
    bool full_repaint = false;
    for (int i = 0; i < nops; i++)
        if (ops[i].op == 'B' && ops[i].dst.x == 0 && ops[i].src.w >= 150)
            full_repaint = true;
    OK(full_repaint);

    /* overlay sibling gets damaged (expanded) when the band shifts */
    surf_node *ship = surf_rect_new(50, 20, 20, 10, 0x1234);
    surf_node_add(surf_screen(), ship);
    surf_tick();
    nops = 0;
    surf_layer_set_offset(l, 108 << 16);
    surf_tick();
    bool ship_redrawn = false;
    for (int i = 0; i < nops; i++)
        if (ops[i].op == 'F' && ops[i].c == 0x1234)
            ship_redrawn = true;
    OK(ship_redrawn);

    /* wrap: offsets normalize into [0, strip_w) */
    surf_layer_set_offset(l, -(40 << 16));
    OK(surf_layer_offset(l) == (256 - 40) << 16);

    surf_node_destroy(ship);
    surf_node_destroy(l);

    /* ---- sprite fast pan (camera window over a big image) ---- */
    fresh(200, 100, 16);
    static uint16_t world_px[64];  /* pixels unused by the mock */
    static const surf_image world = {
        .pixels = world_px, .w = 400, .h = 300, .stride = 800,
        .format = SURF_FMT_RGB565, .opaque = true,
    };
    surf_node *cam = surf_sprite_new(&world, 0, 0);
    surf_sprite_set_src(cam, (surf_rect){50, 50, 200, 100});
    surf_sprite_set_fast_pan(cam, true);
    surf_node_add(surf_screen(), cam);
    surf_node *hero = surf_rect_new(90, 40, 20, 20, 0x4321);
    surf_node_add(surf_screen(), hero);
    surf_tick();

    nops = 0;
    surf_sprite_set_src(cam, (surf_rect){53, 52, 200, 100});
    surf_tick();
    bool pan_shift = false, svert = false, shorz = false, hero_redraw = false;
    for (int i = 0; i < nops; i++) {
        if (ops[i].op == 'S' && ops[i].dst.x == -3 && ops[i].dst.y == -2)
            pan_shift = true;
        if (ops[i].op == 'B' && ops[i].dst.x == 197 && ops[i].src.w == 3)
            svert = true;
        if (ops[i].op == 'B' && ops[i].dst.y == 98 && ops[i].src.h == 2 &&
            ops[i].src.w == 197)
            shorz = true;
        if (ops[i].op == 'F' && ops[i].c == 0x4321)
            hero_redraw = true;
    }
    OK(pan_shift && svert && shorz);
    OK(hero_redraw);

    /* same-value call after a shift: heals with one full repaint */
    nops = 0;
    surf_sprite_set_src(cam, (surf_rect){53, 52, 200, 100});
    surf_tick();
    bool full = false;
    for (int i = 0; i < nops; i++)
        if (ops[i].op == 'B' && ops[i].src.w == 200 && ops[i].src.h == 100)
            full = true;
    OK(full);

    surf_node_destroy(hero);
    surf_node_destroy(cam);
}
