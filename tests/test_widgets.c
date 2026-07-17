/* M1 tests: filmstrip/ninepatch nodes, touch dispatch + capture, slider
 * and knob behavior driven through the real input path (queued touches). */
#include <stdlib.h>

#include "mock_hal.h"

static surf_image strip_img = {
    .pixels = (void *)&strip_img, .w = 40, .h = 10, .stride = 160,
    .format = SURF_FMT_ARGB8888, .opaque = true,
};
static surf_image patch_img = {
    .pixels = (void *)&patch_img, .w = 12, .h = 12, .stride = 48,
    .format = SURF_FMT_ARGB8888, .opaque = true,
};

static void test_filmstrip(void)
{
    fresh(100, 50, 32);

    surf_node *n = surf_filmstrip_new(&strip_img, 10, 10, 0, 0);
    surf_node_add(surf_screen(), n);
    surf_filmstrip_set_frame(n, 2);
    surf_tick();

    /* opaque frame covers its dirty rect: one blit from frame 2's cell */
    OK(nops == 2);
    OK(ops[0].op == 'B' && ops[0].src.x == 20 && ops[0].src.y == 0);
    OK(ops[0].src.w == 10 && ops[0].src.h == 10);

    surf_filmstrip_set_frame(n, 99);  /* clamps to last frame */
    OK(surf_filmstrip_frame(n) == 3);
    OK(surf_g.dirty.n == 1);
    OK(rect_eq(surf_g.dirty.r[0], (surf_rect){0, 0, 10, 10}));
}

static int count_blits(void)
{
    int c = 0;
    for (int i = 0; i < nops; i++)
        if (ops[i].op == 'B')
            c++;
    return c;
}

static void test_ninepatch(void)
{
    fresh(100, 50, 32);

    /* dst == source size: every region is exactly one blit */
    surf_node *n = surf_ninepatch_new(&patch_img, 0, 0, 12, 12, 4, 4, 4, 4);
    surf_node_add(surf_screen(), n);
    surf_tick();
    OK(count_blits() == 9);
    bool corner = false;
    for (int i = 0; i < nops; i++)
        if (ops[i].op == 'B' && ops[i].dst.x == 8 && ops[i].dst.y == 8 &&
            ops[i].src.x == 8 && ops[i].src.y == 8)
            corner = true;
    OK(corner);

    /* stretched: 4px middles tile a 12px span 3× → 4 + 4*3 + 3*3 = 25 */
    surf_ninepatch_set_size(n, 20, 20);
    nops = 0;
    surf_tick();
    OK(count_blits() == 25);
}

/* ---- input dispatch ---- */

static int         ev_count;
static surf_touch  ev_last;
static surf_node  *ev_node;

static void record_touch(surf_node *n, const surf_touch *t, void *user)
{
    (void)user;
    ev_count++;
    ev_last = *t;
    ev_node = n;
}

static void test_input_capture(void)
{
    fresh(200, 100, 32);
    ev_count = 0;

    surf_node *r = surf_rect_new(10, 10, 20, 20, 1);
    surf_node_add(surf_screen(), r);
    surf_node_set_on_touch(r, record_touch, NULL);

    mock_push_touch((surf_touch){15, 15, SURF_TOUCH_DOWN});
    surf_tick();
    OK(ev_count == 1 && ev_last.phase == SURF_TOUCH_DOWN && ev_node == r);

    /* capture: moves outside the rect still arrive */
    mock_push_touch((surf_touch){190, 90, SURF_TOUCH_MOVE});
    mock_push_touch((surf_touch){190, 90, SURF_TOUCH_UP});
    surf_tick();
    OK(ev_count == 3 && ev_last.phase == SURF_TOUCH_UP);

    /* after UP nothing is captured; empty space eats events */
    mock_push_touch((surf_touch){190, 90, SURF_TOUCH_DOWN});
    mock_push_touch((surf_touch){15, 15, SURF_TOUCH_MOVE});
    surf_tick();
    OK(ev_count == 3);

    /* handler on an ancestor group receives leaf hits */
    surf_node *g = surf_group_new(100, 0);
    surf_node *leaf = surf_rect_new(0, 0, 30, 30, 2);
    surf_node_add(g, leaf);
    surf_node_add(surf_screen(), g);
    surf_node_set_on_touch(g, record_touch, NULL);
    mock_push_touch((surf_touch){110, 10, SURF_TOUCH_DOWN});
    surf_tick();
    OK(ev_count == 4 && ev_node == g);

    /* detaching the captured subtree releases capture */
    surf_node_detach(g);
    mock_push_touch((surf_touch){110, 10, SURF_TOUCH_MOVE});
    surf_tick();
    OK(ev_count == 4);
}

/* ---- widgets ---- */

static int32_t change_value;
static int     change_count;

static void record_change(int32_t v, void *user)
{
    (void)user;
    change_value = v;
    change_count++;
}

static surf_image track_img = {
    .pixels = (void *)&track_img, .w = 36, .h = 36, .stride = 144,
    .format = SURF_FMT_ARGB8888, .opaque = false,
};
static surf_image cap_img = {
    .pixels = (void *)&cap_img, .w = 20, .h = 10, .stride = 80,
    .format = SURF_FMT_ARGB8888, .opaque = false,
};

static void test_slider(void)
{
    fresh(200, 200, 32);
    change_count = 0;

    surf_slider_style st = {.track = &track_img, .inset = 12, .cap = &cap_img};
    surf_slider *s = surf_slider_new(surf_screen(), 10, 10, 20, 100, &st);
    OK(s != NULL);
    OK(surf_slider_value(s) == 0);

    /* cap sits at the bottom at value 0 */
    surf_node *cap = surf_hit_test(20, 105);
    OK(cap && cap->type == SURF_NODE_SPRITE);

    /* down mid-track: cap centers on the finger → value 0.5 */
    mock_push_touch((surf_touch){20, 60, SURF_TOUCH_DOWN});
    surf_tick();
    OK(change_count == 0);  /* no cb wired yet */
    OK(surf_slider_value(s) == SURF_ONE / 2);

    surf_slider_on_change(s, record_change, NULL);
    mock_push_touch((surf_touch){20, 15, SURF_TOUCH_MOVE});  /* above top */
    surf_tick();
    OK(surf_slider_value(s) == SURF_ONE);
    OK(change_count == 1 && change_value == SURF_ONE);

    mock_push_touch((surf_touch){20, 199, SURF_TOUCH_MOVE});  /* below bottom */
    mock_push_touch((surf_touch){20, 199, SURF_TOUCH_UP});
    surf_tick();
    OK(surf_slider_value(s) == 0);
    OK(change_count == 2 && change_value == 0);

    /* programmatic set moves the cap but fires no cb */
    surf_slider_set_value(s, SURF_ONE / 4);
    OK(surf_slider_value(s) == SURF_ONE / 4 && change_count == 2);

    surf_slider_destroy(s);
}

static surf_image knobstrip_img = {
    .pixels = (void *)&knobstrip_img, .w = 640, .h = 64, .stride = 2560,
    .format = SURF_FMT_ARGB8888, .opaque = false,
};

static void test_knob(void)
{
    fresh(400, 400, 32);
    change_count = 0;

    surf_knob_style st = {.strip = &knobstrip_img, .frame_w = 64, .frame_h = 64,
                          .frames = 10};
    surf_knob *k = surf_knob_new(surf_screen(), 100, 100, &st);
    OK(k != NULL);
    surf_node *strip = surf_knob_node(k)->first;
    OK(strip && strip->type == SURF_NODE_FILMSTRIP);
    surf_knob_on_change(k, record_change, NULL);

    /* vertical relative drag: 100px up over a 200px range → +0.5 */
    mock_push_touch((surf_touch){132, 132, SURF_TOUCH_DOWN});
    surf_tick();
    OK(surf_knob_value(k) == 0 && change_count == 0);

    mock_push_touch((surf_touch){132, 32, SURF_TOUCH_MOVE});
    surf_tick();
    OK(surf_knob_value(k) == SURF_ONE / 2);
    OK(change_count == 1);
    OK(surf_filmstrip_frame(strip) == 5);  /* round(0.5 * 9) */

    /* keeps tracking way past the widget, clamps at full scale */
    mock_push_touch((surf_touch){132, -68, SURF_TOUCH_MOVE});
    mock_push_touch((surf_touch){132, -68, SURF_TOUCH_UP});
    surf_tick();
    OK(surf_knob_value(k) == SURF_ONE);
    OK(surf_filmstrip_frame(strip) == 9);

    /* angular mode: 3 o'clock = +90° into a ±135° sweep → 5/6 */
    surf_knob_set_value(k, 0);
    surf_knob_set_mode(k, SURF_KNOB_DRAG_ANGULAR);
    mock_push_touch((surf_touch){162, 132, SURF_TOUCH_DOWN});
    mock_push_touch((surf_touch){162, 132, SURF_TOUCH_UP});
    surf_tick();
    int32_t v = surf_knob_value(k);
    OK(v > SURF_ONE * 5 / 6 - 60 && v < SURF_ONE * 5 / 6 + 60);

    /* destroying the widget mid-drag releases capture safely */
    surf_knob_set_mode(k, SURF_KNOB_DRAG_VERTICAL);
    change_count = 0;
    mock_push_touch((surf_touch){132, 132, SURF_TOUCH_DOWN});
    surf_tick();
    surf_knob_destroy(k);
    mock_push_touch((surf_touch){132, 90, SURF_TOUCH_MOVE});
    surf_tick();
    OK(change_count == 0);
}

void run_widget_tests(void)
{
    test_filmstrip();
    test_ninepatch();
    test_input_capture();
    test_slider();
    test_knob();
}
