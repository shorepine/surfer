/* Core unit tests: rect ops, dirty-rect coalescing, damage marking,
 * hit test, compose op stream. Pure C, mock hal, no SDL. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "mock_hal.h"

void run_widget_tests(void);  /* tests/test_widgets.c */
void run_text_tests(void);    /* tests/test_text.c */
void run_scroll_tests(void);  /* tests/test_scroll.c; needs run_text_tests first */

/* ---- tests ---- */

static void test_rect_ops(void)
{
    surf_rect a = {0, 0, 10, 10}, b = {5, 5, 10, 10}, c = {20, 20, 4, 4};

    OK(rect_eq(surf_rect_intersect(a, b), (surf_rect){5, 5, 5, 5}));
    OK(surf_rect_empty(surf_rect_intersect(a, c)));
    OK(rect_eq(surf_rect_union(a, b), (surf_rect){0, 0, 15, 15}));
    OK(rect_eq(surf_rect_union(a, (surf_rect){0, 0, 0, 0}), a));
    OK(surf_rect_overlaps(a, b));
    OK(!surf_rect_overlaps(a, c));
    OK(!surf_rect_overlaps(a, (surf_rect){10, 0, 5, 5}));  /* touching != overlap */
    OK(surf_rect_covers((surf_rect){0, 0, 20, 20}, a));
    OK(!surf_rect_covers(a, (surf_rect){0, 0, 20, 20}));
    OK(surf_rect_contains(a, 0, 0));
    OK(surf_rect_contains(a, 9, 9));
    OK(!surf_rect_contains(a, 10, 10));
}

static void test_dirty_coalesce(void)
{
    surf_dirty d;
    surf_dirty_reset(&d, (surf_rect){0, 0, 400, 400});

    surf_dirty_add(&d, (surf_rect){0, 0, 10, 10});
    surf_dirty_add(&d, (surf_rect){5, 5, 10, 10});
    OK(d.n == 1);
    OK(rect_eq(d.r[0], (surf_rect){0, 0, 15, 15}));

    surf_dirty_add(&d, (surf_rect){100, 100, 10, 10});
    OK(d.n == 2);

    /* a bridging rect collapses newly-overlapping entries transitively */
    surf_dirty_add(&d, (surf_rect){10, 10, 95, 95});
    OK(d.n == 1);
    OK(rect_eq(d.r[0], (surf_rect){0, 0, 110, 110}));

    /* clipped to screen; fully offscreen is dropped */
    surf_dirty_reset(&d, (surf_rect){0, 0, 400, 400});
    surf_dirty_add(&d, (surf_rect){-5, -5, 10, 10});
    OK(d.n == 1 && rect_eq(d.r[0], (surf_rect){0, 0, 5, 5}));
    surf_dirty_add(&d, (surf_rect){500, 500, 10, 10});
    OK(d.n == 1);

    /* overflow degrades to one bounding union, never drops damage */
    surf_dirty_reset(&d, (surf_rect){0, 0, 400, 400});
    for (int i = 0; i < SURF_MAX_DIRTY; i++)
        surf_dirty_add(&d, (surf_rect){(int16_t)(i * 20), (int16_t)(i * 20), 5, 5});
    OK(d.n == SURF_MAX_DIRTY);
    surf_dirty_add(&d, (surf_rect){390, 0, 5, 5});
    OK(d.n == 1);
    OK(rect_eq(d.r[0], (surf_rect){0, 0, 395, 305}));
}

static void test_damage_on_writes(void)
{
    fresh(200, 100, 32);

    surf_node *r = surf_rect_new(10, 10, 20, 20, SURF_RGB(255, 0, 0));
    surf_node_add(surf_screen(), r);
    OK(surf_g.dirty.n == 1);
    OK(rect_eq(surf_g.dirty.r[0], (surf_rect){10, 10, 20, 20}));
    surf_tick();

    /* move: old and new rects both damaged (disjoint → two entries) */
    surf_node_set_pos(r, 100, 50);
    OK(surf_g.dirty.n == 2);
    OK(rect_eq(surf_g.dirty.r[0], (surf_rect){10, 10, 20, 20}));
    OK(rect_eq(surf_g.dirty.r[1], (surf_rect){100, 50, 20, 20}));
    surf_tick();

    /* group offset applies to child damage */
    surf_node *g = surf_group_new(50, 0);
    surf_node *c = surf_rect_new(0, 0, 10, 10, SURF_RGB(0, 255, 0));
    surf_node_add(g, c);
    OK(surf_g.dirty.n == 0);  /* detached edits are free */
    surf_node_add(surf_screen(), g);
    OK(surf_g.dirty.n == 1);
    OK(rect_eq(surf_g.dirty.r[0], (surf_rect){50, 0, 10, 10}));
    surf_tick();

    surf_node_detach(g);
    OK(surf_g.dirty.n == 1);
    OK(rect_eq(surf_g.dirty.r[0], (surf_rect){50, 0, 10, 10}));
    surf_tick();
    OK(!surf_node_attached(g));

    /* reattach: subtree survived detach intact */
    surf_node_add(surf_screen(), g);
    OK(surf_node_attached(c));
    OK(rect_eq(surf_g.dirty.r[0], (surf_rect){50, 0, 10, 10}));
}

static void test_hit(void)
{
    fresh(200, 100, 32);

    surf_node *base = surf_rect_new(0, 0, 100, 100, 1);
    surf_node *top = surf_rect_new(20, 20, 30, 30, 2);
    surf_node_add(surf_screen(), base);
    surf_node_add(surf_screen(), top);

    OK(surf_hit_test(25, 25) == top);   /* frontmost wins */
    OK(surf_hit_test(5, 5) == base);
    OK(surf_hit_test(150, 50) == NULL);

    surf_node_set_hidden(top, true);
    OK(surf_hit_test(25, 25) == base);
    surf_node_set_hidden(top, false);

    /* clip: child extends past the clip rect, hits stop at the clip edge */
    surf_node *g = surf_group_new(0, 0);
    surf_group_set_clip(g, 40, 40);
    surf_node *wide = surf_rect_new(0, 0, 100, 100, 3);
    surf_node_add(g, wide);
    surf_node_add(surf_screen(), g);
    OK(surf_hit_test(10, 10) == wide);
    OK(surf_hit_test(60, 60) == base);

    /* group offsets compound */
    surf_node *outer = surf_group_new(100, 0);
    surf_node *inner = surf_group_new(20, 20);
    surf_node *leaf = surf_rect_new(0, 0, 10, 10, 4);
    surf_node_add(inner, leaf);
    surf_node_add(outer, inner);
    surf_node_add(surf_screen(), outer);
    OK(surf_hit_test(125, 25) == leaf);
    OK(surf_hit_test(125, 35) == NULL);
}

static surf_image alpha_img = {
    .pixels = (void *)&alpha_img, .w = 10, .h = 10, .stride = 40,
    .format = SURF_FMT_ARGB8888, .opaque = false,
};
static surf_image opaque_img = {
    .pixels = (void *)&opaque_img, .w = 10, .h = 10, .stride = 64,
    .format = SURF_FMT_RGB565, .opaque = true,
};

static void test_compose(void)
{
    fresh(100, 50, 32);

    /* empty scene, damage → bg fill + present */
    surf_dirty_add(&surf_g.dirty, (surf_rect){0, 0, 100, 50});
    surf_tick();
    OK(nops == 2);
    OK(ops[0].op == 'F' && rect_eq(ops[0].r, (surf_rect){0, 0, 100, 50}));
    OK(ops[1].op == 'P' && ops[1].nrects == 1);

    /* nothing dirty → no ops at all, not even present */
    nops = 0;
    surf_tick();
    OK(nops == 0);

    /* opaque full-cover rect suppresses the bg fill (occlusion early-out) */
    surf_node *base = surf_rect_new(0, 0, 100, 50, 7);
    surf_node_add(surf_screen(), base);
    nops = 0;
    surf_tick();
    OK(nops == 2);
    OK(ops[0].op == 'F' && ops[0].c == 7);

    /* alpha sprite over the rect: rect painted first, then blend, no bg */
    surf_node *sp = surf_sprite_new(&alpha_img, 5, 5);
    surf_node_add(surf_screen(), sp);
    nops = 0;
    surf_tick();
    OK(nops == 3);
    OK(ops[0].op == 'F' && ops[0].c == 7 && rect_eq(ops[0].r, (surf_rect){5, 5, 10, 10}));
    OK(ops[1].op == 'A' && ops[1].img == &alpha_img);
    OK(ops[1].dst.x == 5 && ops[1].dst.y == 5);

    /* opaque sprite fully covering the dirty rect stops the walk below it */
    surf_node *op_sp = surf_sprite_new(&opaque_img, 5, 5);
    surf_node_add(surf_screen(), op_sp);
    nops = 0;
    surf_tick();
    OK(nops == 2);
    OK(ops[0].op == 'B' && ops[0].img == &opaque_img);

    /* partial overlap: sprite clipped to the dirty rect, src offset shifts */
    surf_node_set_pos(op_sp, 95, 45);  /* damages old {5,5,10,10} + new {95,45,5,5} */
    nops = 0;
    surf_tick();
    bool found_clipped = false;
    for (int i = 0; i < nops; i++)
        if (ops[i].op == 'B' && ops[i].dst.x == 95 && ops[i].dst.y == 45 &&
            ops[i].src.w == 5 && ops[i].src.h == 5 && ops[i].src.x == 0)
            found_clipped = true;
    OK(found_clipped);
}

static void test_pool(void)
{
    fresh(100, 50, 4);  /* root takes one slot */

    surf_node *a = surf_group_new(0, 0);
    surf_node *b = surf_group_new(0, 0);
    surf_node *c = surf_group_new(0, 0);
    OK(a && b && c);
    OK(surf_group_new(0, 0) == NULL);  /* pool exhausted */

    surf_node_destroy(c);
    OK(surf_group_new(0, 0) != NULL);  /* slot recycled */

    /* destroy recurses into children */
    fresh(100, 50, 4);
    surf_node *g = surf_group_new(0, 0);
    surf_node_add(g, surf_rect_new(0, 0, 5, 5, 1));
    surf_node_add(g, surf_rect_new(0, 0, 5, 5, 2));
    OK(surf_group_new(0, 0) == NULL);
    surf_node_destroy(g);
    OK(surf_group_new(0, 0) && surf_group_new(0, 0) && surf_group_new(0, 0));
}

int main(void)
{
    test_rect_ops();
    test_dirty_coalesce();
    test_damage_on_writes();
    test_hit();
    test_compose();
    test_pool();
    run_widget_tests();
    run_text_tests();
    run_scroll_tests();
    surf_deinit();

    printf("%d checks, %d failures\n", test_checks, test_failures);
    return test_failures ? 1 : 0;
}
