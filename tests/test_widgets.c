/* M1 tests: filmstrip/ninepatch nodes, touch dispatch + capture, slider
 * and knob behavior driven through the real input path (queued touches). */
#include <math.h>
#include <stdlib.h>
#include <string.h>

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

    mock_push_touch((surf_touch){15, 15, SURF_TOUCH_DOWN, 0});
    surf_tick();
    OK(ev_count == 1 && ev_last.phase == SURF_TOUCH_DOWN && ev_node == r);

    /* capture: moves outside the rect still arrive */
    mock_push_touch((surf_touch){190, 90, SURF_TOUCH_MOVE, 0});
    mock_push_touch((surf_touch){190, 90, SURF_TOUCH_UP, 0});
    surf_tick();
    OK(ev_count == 3 && ev_last.phase == SURF_TOUCH_UP);

    /* after UP nothing is captured; empty space eats events */
    mock_push_touch((surf_touch){190, 90, SURF_TOUCH_DOWN, 0});
    mock_push_touch((surf_touch){15, 15, SURF_TOUCH_MOVE, 0});
    surf_tick();
    OK(ev_count == 3);

    /* handler on an ancestor group receives leaf hits */
    surf_node *g = surf_group_new(100, 0);
    surf_node *leaf = surf_rect_new(0, 0, 30, 30, 2);
    surf_node_add(g, leaf);
    surf_node_add(surf_screen(), g);
    surf_node_set_on_touch(g, record_touch, NULL);
    mock_push_touch((surf_touch){110, 10, SURF_TOUCH_DOWN, 0});
    surf_tick();
    OK(ev_count == 4 && ev_node == g);

    /* detaching the captured subtree releases capture */
    surf_node_detach(g);
    mock_push_touch((surf_touch){110, 10, SURF_TOUCH_MOVE, 0});
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
    mock_push_touch((surf_touch){20, 60, SURF_TOUCH_DOWN, 0});
    surf_tick();
    OK(change_count == 0);  /* no cb wired yet */
    OK(surf_slider_value(s) == SURF_ONE / 2);

    surf_slider_on_change(s, record_change, NULL);
    mock_push_touch((surf_touch){20, 15, SURF_TOUCH_MOVE, 0});  /* above top */
    surf_tick();
    OK(surf_slider_value(s) == SURF_ONE);
    OK(change_count == 1 && change_value == SURF_ONE);

    mock_push_touch((surf_touch){20, 199, SURF_TOUCH_MOVE, 0});  /* below bottom */
    mock_push_touch((surf_touch){20, 199, SURF_TOUCH_UP, 0});
    surf_tick();
    OK(surf_slider_value(s) == 0);
    OK(change_count == 2 && change_value == 0);

    /* programmatic set moves the cap but fires no cb */
    surf_slider_set_value(s, SURF_ONE / 4);
    OK(surf_slider_value(s) == SURF_ONE / 4 && change_count == 2);

    surf_slider_destroy(s);

    /* exact-size track art becomes a single sprite, not a tiled 9-patch */
    OK(surf_slider_node(NULL) == NULL);
    static surf_image exact_img = {.pixels = (void *)&exact_img, .w = 20, .h = 100,
                                   .stride = 80, .format = SURF_FMT_ARGB8888};
    surf_slider_style est = {.track = &exact_img, .inset = 12, .cap = &cap_img};
    surf_slider *e = surf_slider_new(surf_screen(), 50, 10, 20, 100, &est);
    OK(e && surf_slider_node(e)->first->type == SURF_NODE_SPRITE);
    surf_slider_destroy(e);
}

/* the compact shape: a track NARROWER than the widget, with the handle
 * riding across it. Two things have to hold or it is unusable — the bar
 * keeps its own width instead of being stretched (stretching tiles the
 * groove), and the gutter either side of it is still grabbable. */
static surf_image slimtrack_img = {
    .pixels = (void *)&slimtrack_img, .w = 8, .h = 14, .stride = 32,
    .format = SURF_FMT_ARGB8888, .opaque = false,
};
static surf_image slimcap_img = {
    .pixels = (void *)&slimcap_img, .w = 24, .h = 14, .stride = 96,
    .format = SURF_FMT_ARGB8888, .opaque = false,
};

static void test_slider_compact(void)
{
    fresh(200, 300, 32);
    change_count = 0;

    surf_slider_style st = {.track = &slimtrack_img, .inset = 5,
                            .cap = &slimcap_img};
    surf_slider *s = surf_slider_new(surf_screen(), 40, 20, 24, 200, &st);
    OK(s != NULL);

    /* the bar is the ART'S width, centred in the widget — not stretched */
    surf_node *track = surf_slider_node(s)->first;
    OK(track->w == 8 && track->x == 8);
    OK(track->h == 200);

    /* a tap in the GUTTER still works: 3px inside the box is 5px off the
     * bar, and without the group's clip it lands on nothing at all */
    surf_slider_on_change(s, record_change, NULL);
    mock_push_touch((surf_touch){43, 120, SURF_TOUCH_DOWN, 0});
    surf_tick();
    OK(change_count == 1);
    /* cap centres on the finger: (120 - 20 - 7) of a 186 range, from the
     * bottom up */
    OK(surf_slider_value(s) == (int32_t)(((int64_t)(186 - 93) << 16) / 186));

    /* ...and the clip is the whole declared box, no more: one past its
     * right edge is somebody else's */
    OK(surf_hit_test(64, 120) != surf_slider_node(s));

    surf_slider_destroy(s);
}

/* THREE FINGERS, THREE FADERS. Capture used to be one node for the whole
 * scene, so the first finger down owned the machine and the other two
 * were dropped — reported from the bench as "I can only move one".
 *
 * Every assertion here fails against that: contact 1 keeps driving its
 * own slider while contacts 2 and 3 are live, an UP on one leaves the
 * others captured, and a second finger on an ALREADY-HELD slider is
 * ignored rather than fighting the first for the cap. */
static void test_multitouch(void)
{
    fresh(400, 300, 32);

    surf_slider_style st = {.track = &track_img, .inset = 12, .cap = &cap_img};
    surf_slider *a = surf_slider_new(surf_screen(), 10, 10, 20, 200, &st);
    surf_slider *b = surf_slider_new(surf_screen(), 60, 10, 20, 200, &st);
    surf_slider *c = surf_slider_new(surf_screen(), 110, 10, 20, 200, &st);
    OK(a && b && c);

    /* three fingers land, one on each */
    mock_push_touch((surf_touch){20, 110, SURF_TOUCH_DOWN, 1});
    mock_push_touch((surf_touch){70, 110, SURF_TOUCH_DOWN, 2});
    mock_push_touch((surf_touch){120, 110, SURF_TOUCH_DOWN, 3});
    surf_tick();
    OK(surf_slider_value(a) > 0 && surf_slider_value(b) > 0
       && surf_slider_value(c) > 0);

    /* ...and each drags its OWN slider, at the same time */
    mock_push_touch((surf_touch){20, 15, SURF_TOUCH_MOVE, 1});    /* a to the top */
    mock_push_touch((surf_touch){70, 205, SURF_TOUCH_MOVE, 2});   /* b to the bottom */
    surf_tick();
    OK(surf_slider_value(a) == SURF_ONE);
    OK(surf_slider_value(b) == 0);
    int32_t held = surf_slider_value(c);
    OK(held > 0 && held < SURF_ONE);       /* c untouched by either */

    /* one finger lifts; the others keep their capture */
    mock_push_touch((surf_touch){20, 15, SURF_TOUCH_UP, 1});
    surf_tick();
    mock_push_touch((surf_touch){120, 15, SURF_TOUCH_MOVE, 3});
    surf_tick();
    OK(surf_slider_value(c) == SURF_ONE);
    OK(surf_slider_value(a) == SURF_ONE);  /* the lifted one stayed put */

    /* a MOVE for a contact that never went down does nothing */
    mock_push_touch((surf_touch){70, 15, SURF_TOUCH_MOVE, 7});
    surf_tick();
    OK(surf_slider_value(b) == 0);

    /* a SECOND finger on a slider another one is already holding is
     * ignored — the first keeps it, the way a physical fader behaves */
    mock_push_touch((surf_touch){70, 110, SURF_TOUCH_DOWN, 4});
    surf_tick();
    OK(surf_slider_value(b) == 0);
    mock_push_touch((surf_touch){70, 60, SURF_TOUCH_MOVE, 4});
    surf_tick();
    OK(surf_slider_value(b) == 0);
    /* ...and the finger that DOES own it still works */
    mock_push_touch((surf_touch){70, 60, SURF_TOUCH_MOVE, 2});
    surf_tick();
    OK(surf_slider_value(b) > 0);

    surf_slider_destroy(a);
    surf_slider_destroy(b);
    surf_slider_destroy(c);
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
    mock_push_touch((surf_touch){132, 132, SURF_TOUCH_DOWN, 0});
    surf_tick();
    OK(surf_knob_value(k) == 0 && change_count == 0);

    mock_push_touch((surf_touch){132, 32, SURF_TOUCH_MOVE, 0});
    surf_tick();
    OK(surf_knob_value(k) == SURF_ONE / 2);
    OK(change_count == 1);
    OK(surf_filmstrip_frame(strip) == 5);  /* round(0.5 * 9) */

    /* keeps tracking way past the widget, clamps at full scale */
    mock_push_touch((surf_touch){132, -68, SURF_TOUCH_MOVE, 0});
    mock_push_touch((surf_touch){132, -68, SURF_TOUCH_UP, 0});
    surf_tick();
    OK(surf_knob_value(k) == SURF_ONE);
    OK(surf_filmstrip_frame(strip) == 9);

    /* angular mode: 3 o'clock = +90° into a ±135° sweep → 5/6 */
    surf_knob_set_value(k, 0);
    surf_knob_set_mode(k, SURF_KNOB_DRAG_ANGULAR);
    mock_push_touch((surf_touch){162, 132, SURF_TOUCH_DOWN, 0});
    mock_push_touch((surf_touch){162, 132, SURF_TOUCH_UP, 0});
    surf_tick();
    int32_t v = surf_knob_value(k);
    OK(v > SURF_ONE * 5 / 6 - 60 && v < SURF_ONE * 5 / 6 + 60);

    /* destroying the widget mid-drag releases capture safely */
    surf_knob_set_mode(k, SURF_KNOB_DRAG_VERTICAL);
    change_count = 0;
    mock_push_touch((surf_touch){132, 132, SURF_TOUCH_DOWN, 0});
    surf_tick();
    surf_knob_destroy(k);
    mock_push_touch((surf_touch){132, 90, SURF_TOUCH_MOVE, 0});
    surf_tick();
    OK(change_count == 0);
}

extern surf_font tfont;  /* synthetic font from test_text.c */

static void test_button(void)
{
    fresh(200, 200, 64);
    static surf_image np = {
        .pixels = (void *)&np, .w = 18, .h = 18, .stride = 72,
        .format = SURF_FMT_ARGB8888,
    };
    surf_button_style st = {.normal = &np, .pressed = &np, .inset = 6,
                            .font = &tfont, .text_color = 1};
    surf_button *b = surf_button_new(surf_screen(), 20, 20, 80, 30, &st, "GO");
    OK(b != NULL);

    static int32_t presses;
    presses = 0;
    extern void test_check_cb(int32_t v, void *user);  /* counts via value */
    surf_button_on_press(b, test_check_cb, &presses);

    /* press + release inside fires once */
    mock_push_touch((surf_touch){40, 30, SURF_TOUCH_DOWN, 0});
    mock_push_touch((surf_touch){45, 32, SURF_TOUCH_UP, 0});
    surf_tick();
    OK(presses == SURF_ONE);

    /* press inside, release outside cancels */
    presses = 0;
    mock_push_touch((surf_touch){40, 30, SURF_TOUCH_DOWN, 0});
    mock_push_touch((surf_touch){150, 150, SURF_TOUCH_MOVE, 0});
    mock_push_touch((surf_touch){150, 150, SURF_TOUCH_UP, 0});
    surf_tick();
    OK(presses == 0);

    surf_button_set_label(b, "STOP");
    surf_button_destroy(b);
}

/* Scrollbar: the widget owns no content, only ratios — check the thumb
 * geometry, the hide-when-nothing-to-scroll rule, and that a drag reports
 * a position in the caller's units. */
static int32_t sb_reported;
static void sb_cb(int32_t v, void *user) { (void)user; sb_reported = v; }

static void test_scrollbar(void)
{
    fresh(400, 300, 32);
    surf_image thumb = {.pixels = NULL, .w = 6, .h = 9, .stride = 24,
                        .format = SURF_FMT_ARGB8888};
    surf_scrollbar_style st = {.thumb = &thumb, .track = NULL, .inset = 4};
    surf_scrollbar *sb = surf_scrollbar_new(surf_screen(), 100, 0, 200, true, &st);
    OK(sb != NULL);
    surf_node *root = surf_scrollbar_node(sb);

    /* nothing to scroll: hidden, and a touch does nothing */
    surf_scrollbar_set_range(sb, 10, 10, 0);
    OK(root->flags & SURF_NF_HIDDEN);

    /* 100 rows, 20 visible -> thumb is a fifth of the track */
    surf_scrollbar_on_change(sb, sb_cb, NULL);
    surf_scrollbar_set_range(sb, 100, 20, 0);
    OK(!(root->flags & SURF_NF_HIDDEN));
    OK(surf_scrollbar_pos(sb) == 0);

    /* drag the thumb to the far end: pos saturates at total - visible */
    sb_reported = -1;
    surf_touch down = {.x = 103, .y = 5, .phase = SURF_TOUCH_DOWN, 0};
    surf_touch move = {.x = 103, .y = 400, .phase = SURF_TOUCH_MOVE, 0};
    surf_inject_touch(&down);
    surf_inject_touch(&move);
    OK(surf_scrollbar_pos(sb) == 80);
    OK(sb_reported == 80);

    /* and back to the top */
    surf_touch up = {.x = 103, .y = -50, .phase = SURF_TOUCH_MOVE, 0};
    surf_inject_touch(&up);
    OK(surf_scrollbar_pos(sb) == 0);

    /* set_pos is clamped, and does NOT fire the callback (it is the
     * caller telling the bar where things are, not the user moving it) */
    sb_reported = -1;
    surf_scrollbar_set_pos(sb, 999);
    OK(surf_scrollbar_pos(sb) == 80 && sb_reported == -1);
    surf_scrollbar_destroy(sb);

    /* Horizontal: the 9-patch slices along fixed axes, so it must take
     * the LYING-DOWN capsule and put its insets on the left/right edges.
     * Given the upright art it stretched a round cap along the bar and
     * drew a string of beads. */
    surf_image thumb_h = {.pixels = NULL, .w = 9, .h = 6, .stride = 36,
                          .format = SURF_FMT_ARGB8888};
    surf_scrollbar_style hst = {.thumb = &thumb, .track = NULL, .inset = 4,
                                .thumb_h = &thumb_h};
    surf_scrollbar *hb = surf_scrollbar_new(surf_screen(), 0, 200, 200, false,
                                            &hst);
    OK(hb != NULL);
    surf_node *hthumb = surf_scrollbar_node(hb)->first;
    OK(hthumb->u.nine.img == &thumb_h);                  /* the right art */
    OK(hthumb->u.nine.l == 4 && hthumb->u.nine.r == 4 && /* ...sliced along */
       hthumb->u.nine.t == 0 && hthumb->u.nine.b == 0);  /*    the axis */

    /* and it still reports in the caller's unit when dragged */
    surf_scrollbar_on_change(hb, sb_cb, NULL);
    surf_scrollbar_set_range(hb, 100, 20, 0);
    sb_reported = -1;
    surf_touch hd = {.x = 5, .y = 203, .phase = SURF_TOUCH_DOWN, 0};
    surf_touch hm = {.x = 400, .y = 203, .phase = SURF_TOUCH_MOVE, 0};
    surf_inject_touch(&hd);
    surf_inject_touch(&hm);
    OK(surf_scrollbar_pos(hb) == 80 && sb_reported == 80);
    surf_scrollbar_destroy(hb);
}

static int32_t sel_reported;
static void sel_cb(int32_t v, void *user) { (void)user; sel_reported = v; }

/* An LED reports nothing (it is an output) and a selector reports an
 * INDEX. Both were added for the TB-303 panel. */
static void test_led_and_selector(void)
{
    fresh(400, 300, 32);
    surf_image strip = {.pixels = NULL, .w = 16 * 6, .h = 16, .stride = 96,
                        .format = SURF_FMT_A8};
    surf_led_style ls = {.strip = &strip, .frame_w = 16, .frame_h = 16,
                         .frames = 6, .color = SURF_RGB(255, 0, 0)};
    surf_led *l = surf_led_new(surf_screen(), 10, 10, &ls);
    OK(l != NULL);
    OK(surf_led_level(l) == 0);
    surf_led_set(l, true);
    OK(surf_led_level(l) == SURF_ONE);
    surf_led_set_level(l, SURF_ONE / 2);
    OK(surf_led_level(l) == SURF_ONE / 2);
    surf_led_set_level(l, SURF_ONE * 4);          /* clamps */
    OK(surf_led_level(l) == SURF_ONE);
    /* the tint is PER LED — the style's image is shared and must not be
     * touched, or every lamp on the panel would change colour together */
    surf_led *l2 = surf_led_new(surf_screen(), 40, 10, &ls);
    surf_led_set_color(l2, SURF_RGB(0, 255, 0));
    OK(strip.tint == 0);                          /* the shared art is clean */
    surf_led_destroy(l2);
    surf_led_destroy(l);

    surf_image knob = {.pixels = NULL, .w = 56 * 64, .h = 56,
                       .stride = 56 * 64 * 4, .format = SURF_FMT_ARGB8888};
    surf_knob_style ks = {.strip = &knob, .frame_w = 56, .frame_h = 56,
                          .frames = 64};
    surf_selector *sel = surf_selector_new(surf_screen(), 100, 100, &ks, 4);
    OK(sel != NULL);
    OK(surf_selector_index(sel) == 0 && surf_selector_positions(sel) == 4);
    surf_selector_on_change(sel, sel_cb, NULL);

    /* a TAP (press and release without travel) advances one and wraps */
    sel_reported = -1;
    for (int i = 1; i <= 4; i++) {
        surf_touch d = {.x = 120, .y = 120, .phase = SURF_TOUCH_DOWN, 0};
        surf_touch u = {.x = 120, .y = 120, .phase = SURF_TOUCH_UP, 0};
        surf_inject_touch(&d);
        surf_inject_touch(&u);
        OK(surf_selector_index(sel) == i % 4);
    }
    OK(sel_reported == 0);                        /* wrapped back to 0 */

    /* a DRAG snaps through positions and never lands between them. The
     * sweep is SEL_DRAG_RANGE (160px) for the whole span, so 160px up
     * from position 0 is position 3 — a shorter drag lands proportionally
     * short, which is the thing a detent is for. */
    surf_selector_set_index(sel, 0);
    /* the press has to LAND on the widget (100,100 56x56) — capture then
     * keeps the drag alive off the top of the screen */
    surf_touch d = {.x = 120, .y = 150, .phase = SURF_TOUCH_DOWN, 0};
    surf_inject_touch(&d);
    bool saw_middle = false;
    for (int y = 145; y >= -10; y -= 5) {
        surf_touch m = {.x = 120, .y = (int16_t)y, .phase = SURF_TOUCH_MOVE, 0};
        surf_inject_touch(&m);
        int32_t i = surf_selector_index(sel);
        OK(i >= 0 && i <= 3);
        if (i == 1 || i == 2)
            saw_middle = true;
    }
    OK(saw_middle);                               /* it stepped, not jumped */
    OK(surf_selector_index(sel) == 3);            /* and saturates at the top */
    surf_touch u2 = {.x = 120, .y = -10, .phase = SURF_TOUCH_UP, 0};
    surf_inject_touch(&u2);
    OK(surf_selector_index(sel) == 3);            /* a drag is not a tap */

    /* set_index is the caller talking: no callback, same as every widget */
    sel_reported = -1;
    surf_selector_set_index(sel, 1);
    OK(surf_selector_index(sel) == 1 && sel_reported == -1);
    surf_selector_destroy(sel);
}

static int32_t cp_reported;
static void cp_cb(int32_t v, void *user) { (void)user; cp_reported = v; }

/* The colour picker draws its own art, so what a test can check is the
 * arithmetic: where a press lands in HSV, and that a colour set from
 * outside comes back. */
static void test_colorpicker(void)
{
    fresh(400, 300, 64);
    surf_colorpicker *c = surf_colorpicker_new(surf_screen(), 10, 10, 100);
    OK(c != NULL);
    /* top-right of the square at hue 0 is full red */
    OK(surf_colorpicker_color(c) == SURF_RGB(255, 0, 0));
    surf_colorpicker_on_change(c, cp_cb, NULL);

    /* bottom-left: no saturation, no value — black, whatever the hue */
    cp_reported = -1;
    surf_touch d = {.x = 11, .y = 108, .phase = SURF_TOUCH_DOWN, 0};
    surf_inject_touch(&d);
    OK(surf_colorpicker_color(c) == 0);
    OK(cp_reported == 0);

    /* top-left: no saturation, full value — white */
    surf_touch m = {.x = 10, .y = 10, .phase = SURF_TOUCH_MOVE, 0};
    surf_inject_touch(&m);
    OK(surf_colorpicker_color(c) == SURF_RGB(255, 255, 255));

    /* a colour set from outside round-trips to about itself (565 has
     * five bits of red, so "about" is the honest word) */
    surf_colorpicker_set_color(c, SURF_RGB(60, 180, 240));
    surf_color got = surf_colorpicker_color(c);
    int dr = ((got >> 11) & 31) - ((SURF_RGB(60, 180, 240) >> 11) & 31);
    int dg = ((got >> 5) & 63) - ((SURF_RGB(60, 180, 240) >> 5) & 63);
    OK(dr >= -1 && dr <= 1 && dg >= -1 && dg <= 1);
    surf_colorpicker_destroy(c);
}

/* A masked field draws the SAME glyph for every character. The mock hal
 * records every blend, so that is directly checkable: four characters,
 * four blends, all from one atlas cell.
 *
 * Its own font, not test_text.c's: that one is filled in by a mkfont()
 * that runs when the TEXT suite does, and this suite runs first — so
 * borrowing it meant a font of all zeros, a node 200x0 tall, and nothing
 * painted at all. */
static surf_glyph mg[5];
static uint8_t matlas[64 * 16];
static surf_font mfont;

static void mkmaskfont(void)
{
    for (int i = 0; i < 4; i++)          /* A B C D, each its own cell */
        mg[i] = (surf_glyph){(uint32_t)('A' + i), (int16_t)(i * 8), 0,
                             8, 10, 1, -10, 10};
    mg[4] = (surf_glyph){'*', 32, 0, 8, 10, 1, -10, 10};
    memset(matlas, 0x80, sizeof matlas);
    mfont = (surf_font){
        .atlas = {.pixels = matlas, .w = 64, .h = 16, .stride = 64,
                  .format = SURF_FMT_A8},
        .ascent = 12, .descent = -3, .line_gap = 1,
        .glyphs = mg, .nglyphs = 5,
    };
}

static void test_textinput_mask(void)
{
    mkmaskfont();
    fresh(400, 300, 32);
    surf_node *n = surf_textinput_new(&mfont, 0, 0, 200, SURF_RGB(255, 255, 255));
    surf_node_add(surf_screen(), n);
    surf_textinput_set_text(n, "ABCD");
    surf_tick();

    nops = 0;
    surf_textinput_set_mask(n, 'A');
    OK(surf_textinput_mask(n) == 'A');
    surf_tick();
    int blits = 0;
    int16_t src0 = -1;
    bool same = true;
    for (int i = 0; i < nops; i++)
        if (ops[i].op == 'A') {
            if (blits++ == 0)
                src0 = ops[i].src.x;
            else if (ops[i].src.x != src0)
                same = false;
        }
    OK(blits == 4);            /* one per character... */
    OK(same);                  /* ...and every one the same glyph */

    /* the buffer is untouched: a mask, not a cipher */
    const char *t = surf_textinput_text(n);
    OK(t[0] == 'A' && t[1] == 'B' && t[2] == 'C' && t[3] == 'D');

    /* and the caret still indexes the real text, measured over the mask */
    surf_textinput_set_caret(n, 2, false);
    OK(surf_textinput_caret(n) == 2);
    OK(surf_textinput_index_from_x(n, 1000) == 4);

    /* unmasking draws the real glyphs again — four DIFFERENT cells */
    nops = 0;
    surf_textinput_set_mask(n, 0);
    surf_tick();
    blits = 0;
    same = true;
    src0 = -1;
    for (int i = 0; i < nops; i++)
        if (ops[i].op == 'A') {
            if (blits++ == 0)
                src0 = ops[i].src.x;
            else if (ops[i].src.x != src0)
                same = false;
        }
    OK(blits == 4 && !same);
    surf_node_destroy(n);
}

static int32_t tab_reported;
static void tab_cb(int32_t v, void *user) { (void)user; tab_reported = v; }

/* Does a rect of this colour reach the screen this frame? `hidden` is
 * write-only on a node — deliberately, and tulip5 leans on it — so the
 * only honest way to ask "is that page showing" is to compose and see
 * what the hal was told to fill. */
static bool filled(surf_color c)
{
    for (int i = 0; i < nops; i++)
        if (ops[i].op == 'F' && ops[i].c == c)
            return true;
    return false;
}

/* Tabs: the strip picks, and the WIDGET does the hiding. That second
 * half is the whole reason it is a widget rather than four lines in a
 * caller, so it is what this checks. */
static void test_tabs(void)
{
    fresh(400, 300, 32);
    surf_image face = {.pixels = NULL, .w = 24, .h = 24, .stride = 24,
                       .format = SURF_FMT_A8};
    /* the synthetic face test_text.c builds — the built-in registry is
       a generated file the unit suite does not link */
    surf_tabs_style st = {.patch = &face, .inset_side = 10, .inset_top = 10,
                          .inset_bottom = 2, .font = &tfont,
                          .face = SURF_RGB(40, 40, 48),
                          .dim = SURF_RGB(20, 20, 24),
                          .text_active = 1, .text = 1};
    const char *labels[3] = {"one", "two", "three"};
    surf_tabs *t = surf_tabs_new(surf_screen(), 0, 0, 300, 200, 40, &st,
                                 labels, 3);
    OK(t != NULL);
    OK(surf_tabs_count(t) == 3 && surf_tabs_index(t) == 0);
    surf_tabs_on_change(t, tab_cb, NULL);

    /* one page per tab, stable across calls */
    surf_node *p0 = surf_tabs_page(t, 0), *p2 = surf_tabs_page(t, 2);
    OK(p0 && p2 && p0 != p2 && surf_tabs_page(t, 0) == p0);
    OK(surf_tabs_page(t, 3) == NULL && surf_tabs_page(t, -1) == NULL);

    /* something recognisable on two of the pages */
    /* BRIGHT and distinct: these are packed to 565 on the way to the
       hal, so SURF_RGB(1, 2, 3) would arrive as 0 — the same value the
       screen is cleared to, and a check that can never fail. */
    const surf_color C0 = SURF_RGB(255, 0, 0), C2 = SURF_RGB(0, 255, 0);
    surf_node_add(p0, surf_rect_new(0, 0, 100, 40, C0));
    surf_node_add(p2, surf_rect_new(0, 0, 100, 40, C2));
    nops = 0;
    surf_tick();
    OK(filled(C0) && !filled(C2));            /* page 0 is up */

    /* a tap on the third tab: 300/3 = 100 wide each, so x=250 is in it */
    tab_reported = -1;
    surf_touch d = {.x = 250, .y = 20, .phase = SURF_TOUCH_DOWN, 0};
    surf_touch u = {.x = 250, .y = 20, .phase = SURF_TOUCH_UP, 0};
    surf_inject_touch(&d);
    surf_inject_touch(&u);
    OK(surf_tabs_index(t) == 2 && tab_reported == 2);
    nops = 0;
    surf_tick();
    OK(filled(C2) && !filled(C0));            /* ...and the pages swapped */

    /* a tap BELOW the strip is the page's business, not the strip's: a
     * tab bar that switched on a touch anywhere in the panel would make
     * every control under it change the page */
    tab_reported = -1;
    surf_touch pd = {.x = 50, .y = 120, .phase = SURF_TOUCH_DOWN, 0};
    surf_touch pu = {.x = 50, .y = 120, .phase = SURF_TOUCH_UP, 0};
    surf_inject_touch(&pd);
    surf_inject_touch(&pu);
    OK(surf_tabs_index(t) == 2 && tab_reported == -1);

    /* set_index does NOT report — the caller already knows, and every
     * other widget's setter follows the same rule */
    surf_tabs_set_index(t, 0);
    OK(surf_tabs_index(t) == 0 && tab_reported == -1);
    nops = 0;
    surf_tick();
    OK(filled(C0) && !filled(C2));
    surf_tabs_set_index(t, 99);                   /* out of range: ignored */
    OK(surf_tabs_index(t) == 0);

    surf_tabs_set_label(t, 1, "renamed");         /* no crash, no reindex */
    OK(surf_tabs_index(t) == 0);

    /* whatever the caller put in a page goes when the tabs do */
    surf_tabs_destroy(t);
}

static int32_t radio_reported;
static void radio_cb(int32_t v, void *user) { (void)user; radio_reported = v; }

/* Radio: one of N, in a column or a row. The row is the case worth
 * testing — its options are as wide as their LABELS, so "which one is
 * under this x" is measured rather than a pitch. */
static void test_radio(void)
{
    fresh(400, 300, 32);
    surf_image strip = {.pixels = NULL, .w = 24 * 2, .h = 24,
                        .stride = 24 * 2 * 4, .format = SURF_FMT_ARGB8888};
    surf_radio_style st = {.strip = &strip, .frame_w = 24, .frame_h = 24,
                           .font = &tfont, .text_color = 1, .gap = 10};
    const char *labels[3] = {"one", "two", "three"};

    surf_radio *v = surf_radio_new(surf_screen(), 10, 10, &st, labels, 3, true);
    OK(v != NULL && surf_radio_count(v) == 3 && surf_radio_index(v) == 0);
    surf_radio_on_change(v, radio_cb, NULL);
    surf_point sz = surf_radio_size(v);
    OK(sz.x > 24 && sz.y >= 24 * 3);          /* three rows, label-wide */

    /* the second row: a row is frame_h tall and gap apart */
    radio_reported = -1;
    surf_touch d = {.x = 20, .y = 10 + 24 + 10 + 5, .phase = SURF_TOUCH_DOWN, 0};
    surf_touch u = {.x = 20, .y = 10 + 24 + 10 + 5, .phase = SURF_TOUCH_UP, 0};
    surf_inject_touch(&d);
    surf_inject_touch(&u);
    OK(surf_radio_index(v) == 1 && radio_reported == 1);

    /* set_index does NOT report, like every other widget here */
    radio_reported = -1;
    surf_radio_set_index(v, 2);
    OK(surf_radio_index(v) == 2 && radio_reported == -1);
    surf_radio_set_index(v, 9);                /* out of range: ignored */
    OK(surf_radio_index(v) == 2);

    /* ...and the ROW. Its width is the sum of its labels, so tapping
     * past the first option's own extent must land on the second. */
    surf_radio *h = surf_radio_new(surf_screen(), 10, 200, &st, labels, 3,
                                   false);
    OK(h != NULL);
    surf_point hs = surf_radio_size(h);
    OK(hs.x > sz.x && hs.y <= 24 + 4);         /* wide and one row tall */
    surf_radio_on_change(h, radio_cb, NULL);
    radio_reported = -1;
    surf_touch d2 = {.x = (int16_t)(10 + hs.x - 20), .y = 205,
                     .phase = SURF_TOUCH_DOWN, 0};
    surf_touch u2 = {.x = (int16_t)(10 + hs.x - 20), .y = 205,
                     .phase = SURF_TOUCH_UP, 0};
    surf_inject_touch(&d2);
    surf_inject_touch(&u2);
    OK(surf_radio_index(h) == 2 && radio_reported == 2);

    /* a tap outside changes nothing */
    radio_reported = -1;
    surf_touch d3 = {.x = 380, .y = 205, .phase = SURF_TOUCH_DOWN, 0};
    surf_touch u3 = {.x = 380, .y = 205, .phase = SURF_TOUCH_UP, 0};
    surf_inject_touch(&d3);
    surf_inject_touch(&u3);
    OK(surf_radio_index(h) == 2 && radio_reported == -1);

    surf_radio_destroy(h);
    surf_radio_destroy(v);
}

/* A handler that destroys the tree it is in, DURING a scrollview's
 * gesture steal. tulip5's app launcher does exactly this: a row treats
 * the synthetic UP as a tap, closes the popup, and the scrollview the
 * dispatch was about to capture is freed under it.
 *
 * On a desktop heap the recycled slot was benign and nobody noticed. In
 * wasm it is not: "RuntimeError: Out of bounds memory access
 * (evaluating getWasmTableEntry(index)(a1,a2,a3))" — a call through a
 * garbage function index, which takes the whole machine with it. */
static surf_node *doomed_group;
static int doomed_calls, ghost_calls;

static void ghost_touch(surf_node *n, const surf_touch *t, void *user)
{
    (void)n; (void)t; (void)user;
    ghost_calls++;
}

static void doomed_row_touch(surf_node *n, const surf_touch *t, void *user)
{
    (void)n; (void)user;
    doomed_calls++;
    if (t->phase == SURF_TOUCH_UP && doomed_group) {
        surf_node_destroy(doomed_group);      /* ...including OURSELVES */
        doomed_group = NULL;
        /* AND THEN ALLOCATE, which is what makes this test bite on a
         * desktop heap: the freed slots go back on the pool's free list,
         * so these take them over. A dispatch still holding the old
         * pointer now calls THIS handler — observable, rather than
         * depending on what a recycled slot happens to contain. (In wasm
         * what it contained was a garbage function index and the whole
         * VM trapped, which is how tulip5 found it.) */
        for (int i = 0; i < 8; i++) {
            surf_node *ghost = surf_group_new(0, 0);
            surf_group_set_clip(ghost, 200, 20);
            surf_node_set_on_touch(ghost, ghost_touch, NULL);
            surf_node_add(surf_screen(), ghost);
        }
    }
}

static void test_destroy_during_steal(void)
{
    fresh(400, 300, 32);
    doomed_group = surf_group_new(0, 0);
    surf_group_set_clip(doomed_group, 200, 100);
    surf_node_add(surf_screen(), doomed_group);
    surf_node *sv = surf_scrollview_new(0, 0, 200, 100);
    surf_node_add(doomed_group, sv);
    for (int i = 0; i < 20; i++) {           /* content taller than view */
        surf_node *row = surf_group_new(0, (int16_t)(i * 20));
        surf_group_set_clip(row, 200, 20);
        surf_node_set_on_touch(row, doomed_row_touch, NULL);
        /* a clipped group with no children has EMPTY bounds, so the
         * scrollview would see no content and never steal anything —
         * which is a fact about surf_node_subtree_bounds worth knowing
         * when a list mysteriously refuses to scroll */
        surf_node_add(row, surf_rect_new(0, 0, 200, 19, SURF_RGB(20, 20, 30)));
        surf_node_add(sv, row);
    }
    surf_tick();

    doomed_calls = 0;
    ghost_calls = 0;
    surf_touch d = {.x = 50, .y = 10, .phase = SURF_TOUCH_DOWN, 0};
    surf_inject_touch(&d);
    OK(doomed_calls == 1);
    /* nine pixels: past STEAL_PX, so the scrollview takes the gesture
     * and the row gets a synthetic UP — which destroys everything */
    surf_touch m = {.x = 50, .y = 19, .phase = SURF_TOUCH_MOVE, 0};
    surf_inject_touch(&m);
    OK(doomed_group == NULL);

    /* the moves after it must reach nobody at all, rather than a freed
     * scrollview: this is the line that used to crash */
    surf_touch m2 = {.x = 50, .y = 40, .phase = SURF_TOUCH_MOVE, 0};
    surf_touch u = {.x = 50, .y = 40, .phase = SURF_TOUCH_UP, 0};
    surf_inject_touch(&m2);
    surf_inject_touch(&u);
    surf_tick();
    /* nothing may have been delivered through the freed capture */
    OK(ghost_calls == 0);
}


/* The knob and selector strips are BAKED, on first use, from a port of
 * the generator's faces — and the port is held to the generator BYTE FOR
 * BYTE: widget_art_ref.h is the generator's own output for the three
 * sizes (test build only; it is the 565 KB the runtime bake exists not
 * to ship). A unit of slack per pixel is allowed because the C is float
 * where the Python is double and an ink value on a truncation boundary
 * may land a unit off on some libm; on this one it is zero. */
#include "widget_art_ref.h"

static void same_strip(const surf_image *s, const uint8_t *ref, int size)
{
    int maxd = 0, differ = 0;
    for (int y = 0; y < size; y++)
        for (int x = 0; x < size * SURF_ART_FRAMES; x++) {
            int d = ((const uint8_t *)s->pixels)[y * s->stride + x] -
                    ref[y * size * SURF_ART_FRAMES + x];
            if (d < 0) d = -d;
            if (d) differ++;
            if (d > maxd) maxd = d;
        }
    OK(maxd <= 1);
    OK(differ * 1000 < size * size * SURF_ART_FRAMES);   /* under 0.1% */
}

/* The A8 value at a quarter-size radius along the pointer's own line
 * for a frame, and at that point's mirror across the vertical axis: on
 * the pointer reads bright, off it reads the body. "Brightest pixel in
 * the frame" is the wrong probe — the knob's specular highlight outranks
 * the pointer, which is how the first cut of this test was wrong. */
static void pointer_probe(const surf_image *s, int size, int frame, int *on, int *off)
{
    double ang = (-135.0 + 270.0 * frame / (SURF_ART_FRAMES - 1)) * 3.14159265 / 180.0;
    double c = size / 2.0, rr = size * 0.25;
    int x = (int)(c + sin(ang) * rr), y = (int)(c - cos(ang) * rr);
    const uint8_t *px = s->pixels;
    *on = px[y * s->stride + frame * size + x];
    *off = px[y * s->stride + frame * size + (size - 1 - x)];
}

static void test_art_strips(void)
{
    fresh(100, 100, 16);
    const surf_image *k = surf_art_knob_strip(64);
    OK(k && k->format == SURF_FMT_A8 && k->w == 64 * SURF_ART_FRAMES && k->h == 64);
    OK(surf_art_knob_strip(64) == k);              /* cached, not rebaked */
    OK(surf_art_knob_strip(40) != k);              /* a size is its own strip */
    OK(surf_art_knob_strip(4) == NULL);
    const uint8_t *px = k->pixels;
    OK(px[0] == 0 && px[63] == 0);                 /* corners are outside the disc */
    OK(px[32 * k->stride + 32] > 0);               /* the middle is ink */
    /* the pointer sweeps: bright on its line, body-grey across from it,
     * at the first frame, mid-sweep (straight up: on == off there) and
     * the last */
    int on, off;
    pointer_probe(k, 64, 0, &on, &off);
    OK(on > 200 && off < 150);
    pointer_probe(k, 64, SURF_ART_FRAMES - 1, &on, &off);
    OK(on > 200 && off < 150);
    pointer_probe(k, 64, 31, &on, &off);           /* 2 deg short of up */
    OK(on > 200 && off > 200);
    OK(mock_sync_calls >= 1);                      /* flushed for a DMA blitter */

    const surf_image *s = surf_art_selector_strip(56);
    OK(s && s->format == SURF_FMT_A8 && s->w == 56 * SURF_ART_FRAMES && s->h == 56);
    OK(s != k && surf_art_selector_strip(56) == s);
    pointer_probe(s, 56, 0, &on, &off);            /* the wedge, not the body */
    OK(on > 230 && off < 200);
    pointer_probe(s, 56, SURF_ART_FRAMES - 1, &on, &off);
    OK(on > 230 && off < 200);

    same_strip(k, ref_knob_px, 64);
    same_strip(surf_art_knob_strip(40), ref_knobsm_px, 40);
    same_strip(s, ref_sel_px, 56);
}

void run_widget_tests(void)
{
    test_art_strips();
    test_colorpicker();
    test_textinput_mask();
    test_led_and_selector();
    test_tabs();
    test_radio();
    test_destroy_during_steal();
    test_scrollbar();
    test_filmstrip();
    test_ninepatch();
    test_input_capture();
    test_slider();
    test_slider_compact();
    test_multitouch();
    test_knob();
}

void run_button_tests(void)  /* needs tfont: runs after run_text_tests */
{
    test_button();
}
