/* M4 tests: scrollview compose/hit/damage through the content offset,
 * gesture steal vs grab, momentum decay, edge spring-back, checkbox,
 * dropdown popup lifecycle. Touches go through the real dispatch path. */
#include <string.h>

#include "mock_hal.h"

static void drag(int16_t x0, int16_t y0, int16_t x1, int16_t y1, int steps)
{
    mock_push_touch((surf_touch){x0, y0, SURF_TOUCH_DOWN});
    surf_tick();
    for (int i = 1; i <= steps; i++) {
        mock_push_touch((surf_touch){
            (int16_t)(x0 + (x1 - x0) * i / steps),
            (int16_t)(y0 + (y1 - y0) * i / steps), SURF_TOUCH_MOVE});
        surf_tick();
    }
    mock_push_touch((surf_touch){x1, y1, SURF_TOUCH_UP});
    surf_tick();
}

static surf_image op_img = {
    .pixels = (void *)&op_img, .w = 40, .h = 40, .stride = 160,
    .format = SURF_FMT_ARGB8888, .opaque = true,
};

static void test_scroll_compose_hit(void)
{
    fresh(200, 200, 64);

    surf_node *sv = surf_scrollview_new(10, 10, 100, 100);
    surf_node_add(surf_screen(), sv);
    surf_node *a = surf_rect_new(0, 0, 40, 40, 1);
    surf_node *b = surf_rect_new(0, 150, 40, 40, 2);  /* below the fold */
    surf_node_add(sv, a);
    surf_node_add(sv, b);
    OK(surf_scrollview_content_size(sv).y == 190);

    surf_tick();
    OK(surf_hit_test(20, 20) == a);
    OK(surf_hit_test(20, 90) == sv);   /* empty space hits the scrollview */
    OK(surf_hit_test(5, 5) == NULL);   /* outside it: nothing */

    surf_scrollview_set_offset(sv, 0, 60);
    OK(surf_scrollview_offset(sv).y == 60);
    nops = 0;
    surf_tick();
    /* b now visible at screen y = 10 + 150 - 60 = 100 */
    OK(surf_hit_test(20, 105) == b);
    OK(surf_hit_test(20, 20) == sv);   /* a scrolled away */
    bool b_painted = false;
    for (int i = 0; i < nops; i++)
        if (ops[i].op == 'F' && ops[i].c == 2 && ops[i].r.y == 100)
            b_painted = true;
    OK(b_painted);

    /* damage from a child clips to the viewport box */
    surf_node_set_pos(b, 0, 155);
    OK(surf_g.dirty.n >= 1);
    surf_rect box = {10, 10, 100, 100};
    for (int i = 0; i < surf_g.dirty.n; i++)
        OK(surf_rect_covers(box, surf_g.dirty.r[i]));
    surf_tick();

    /* clamped programmatic offsets */
    surf_scrollview_set_offset(sv, 0, 999);
    OK(surf_scrollview_offset(sv).y == 95);  /* content 195 - viewport 100 */
    surf_scrollview_set_offset(sv, 0, 0);
    surf_tick();
}

static void test_scroll_drag_steal(void)
{
    fresh(200, 200, 64);

    surf_node *sv = surf_scrollview_new(0, 0, 100, 100);
    surf_node_add(surf_screen(), sv);
    surf_node *tall = surf_rect_new(0, 0, 100, 300, 1);
    surf_node_add(sv, tall);

    /* a direct drag on empty space scrolls immediately — the 8px threshold
     * only gates stealing from a child handler (plus flick drift after UP) */
    drag(50, 80, 50, 75, 2);
    OK(surf_scrollview_offset(sv).y >= 5);
    while (surf_g.nscrollers > 0)
        surf_tick();
    surf_scrollview_set_offset(sv, 0, 0);
    surf_tick();

    /* real drag on a plain leaf scrolls (leaf has no handler) */
    drag(50, 80, 50, 30, 5);
    OK(surf_scrollview_offset(sv).y >= 40);  /* ~50px of finger travel */
    int16_t after = surf_scrollview_offset(sv).y;

    /* momentum: offset keeps growing after UP, then decays to rest */
    int16_t prev = after;
    bool moved_after_up = false;
    for (int i = 0; i < 300 && surf_g.nscrollers > 0; i++) {
        surf_tick();
        if (surf_scrollview_offset(sv).y != prev)
            moved_after_up = true;
        prev = surf_scrollview_offset(sv).y;
    }
    OK(moved_after_up);
    OK(surf_g.nscrollers == 0);
    OK(surf_scrollview_offset(sv).y <= 200);  /* clamped inside content */

    /* handler child without grab: steal takes over after the threshold */
    fresh(200, 200, 64);
    sv = surf_scrollview_new(0, 0, 100, 100);
    surf_node_add(surf_screen(), sv);
    surf_node *btn = surf_rect_new(10, 10, 60, 200, 3);
    surf_node_add(sv, btn);

    static int down_n;
    extern void test_scroll_btn_handler(surf_node *, const surf_touch *, void *);
    surf_node_set_on_touch(btn, test_scroll_btn_handler, &down_n);

    down_n = 0;
    drag(40, 80, 40, 20, 6);
    OK(down_n >= 2);  /* got DOWN and a synthetic UP */
    OK(surf_scrollview_offset(sv).y > 20);  /* scroll stole and moved */

    /* grabbed child: same drag, no steal */
    fresh(200, 200, 64);
    sv = surf_scrollview_new(0, 0, 100, 100);
    surf_node_add(surf_screen(), sv);
    btn = surf_rect_new(10, 10, 60, 200, 3);
    surf_node_set_on_touch(btn, test_scroll_btn_handler, &down_n);
    surf_node_set_gesture_grab(btn, true);
    surf_node_add(sv, btn);
    down_n = 0;
    drag(40, 80, 40, 20, 6);
    OK(surf_scrollview_offset(sv).y == 0);
    OK(down_n >= 8);  /* DOWN + all MOVEs + real UP delivered */
}

void test_scroll_btn_handler(surf_node *n, const surf_touch *t, void *user)
{
    (void)n; (void)t;
    (*(int *)user)++;
}

static void test_axis_lock(void)
{
    fresh(200, 200, 64);
    surf_node *sv = surf_scrollview_new(0, 0, 100, 100);
    surf_node_add(surf_screen(), sv);
    /* content taller than the viewport but narrower: only y scrolls */
    surf_node_add(sv, surf_rect_new(0, 0, 80, 300, 1));

    /* a diagonal drag moves y and leaves x pinned — no sideways wiggle */
    drag(50, 80, 10, 30, 5);
    OK(surf_scrollview_offset(sv).x == 0);
    OK(surf_scrollview_offset(sv).y > 0);
    while (surf_g.nscrollers > 0)
        surf_tick();
    OK(surf_scrollview_offset(sv).x == 0);
}

static void test_fast_scrollview(void)
{
    fresh(200, 200, 64);
    surf_node *sv = surf_scrollview_new(10, 20, 100, 100);
    surf_node_add(surf_screen(), sv);
    surf_node_add(sv, surf_rect_new(0, 0, 80, 300, 1));
    surf_tick();

    surf_scrollview_set_fast_scroll(sv, true);
    nops = 0;
    surf_scrollview_set_offset(sv, 0, 30);
    OK(nops == 1 && ops[0].op == 'S');
    OK(rect_eq(ops[0].r, (surf_rect){10, 20, 100, 100}));
    OK((int16_t)ops[0].c == 30);
    /* only the exposed bottom strip is dirty */
    OK(surf_g.dirty.n == 1 &&
       rect_eq(surf_g.dirty.r[0], (surf_rect){10, 90, 100, 30}));
    surf_tick();

    /* scrolling back up exposes a top strip */
    surf_scrollview_set_offset(sv, 0, 10);
    OK(surf_g.dirty.n == 1 &&
       rect_eq(surf_g.dirty.r[0], (surf_rect){10, 20, 100, 20}));
    surf_tick();

    /* a drag through real dispatch also rides the shift path */
    nops = 0;
    drag(50, 100, 50, 60, 4);
    bool shifted = false, full = false;
    for (int i = 0; i < nops; i++) {
        if (ops[i].op == 'S')
            shifted = true;
        if (ops[i].op == 'F' && ops[i].r.h >= 100)
            full = true;
    }
    OK(shifted);
    OK(!full);  /* never repainted the whole viewport */
    while (surf_g.nscrollers > 0)
        surf_tick();
    surf_node_destroy(sv);
}

static void test_overscroll_spring(void)
{
    fresh(200, 200, 64);
    surf_node *sv = surf_scrollview_new(0, 0, 100, 100);
    surf_node_add(surf_screen(), sv);
    surf_node_add(sv, surf_rect_new(0, 0, 100, 300, 1));

    /* drag well past the top edge: resisted overscroll, then spring back */
    drag(50, 30, 50, 90, 4);  /* downward drag = negative offset */
    /* during the gesture offset went negative; after UP it springs to 0 */
    for (int i = 0; i < 200 && surf_g.nscrollers > 0; i++)
        surf_tick();
    OK(surf_scrollview_offset(sv).y == 0);
    OK(surf_g.nscrollers == 0);
}

static void test_checkbox(void)
{
    fresh(200, 200, 64);
    static surf_image strip = {
        .pixels = (void *)&strip, .w = 56, .h = 28, .stride = 224,
        .format = SURF_FMT_ARGB8888,
    };
    surf_checkbox_style st = {.strip = &strip, .frame_w = 28, .frame_h = 28};
    surf_checkbox *c = surf_checkbox_new(surf_screen(), 10, 10, &st);
    OK(c && !surf_checkbox_checked(c));

    static int32_t got = -1;
    extern void test_check_cb(int32_t v, void *user);
    surf_checkbox_on_change(c, test_check_cb, &got);

    /* tap toggles */
    drag(20, 20, 20, 20, 1);
    OK(surf_checkbox_checked(c) && got == SURF_ONE);
    /* release outside cancels */
    drag(20, 20, 90, 90, 3);
    OK(surf_checkbox_checked(c));
    /* programmatic set fires no cb */
    got = -1;
    surf_checkbox_set_checked(c, false);
    OK(!surf_checkbox_checked(c) && got == -1);
    surf_checkbox_destroy(c);
}

void test_check_cb(int32_t v, void *user)
{
    *(int32_t *)user = v;
}

/* reuse the synthetic font from test_text.c */
extern surf_font tfont;

static int32_t dd_got = -1;
void test_dd_cb(int32_t idx, void *user)
{
    (void)user;
    dd_got = idx;
}

static void test_dropdown(void)
{
    fresh(400, 300, 128);
    static surf_image panel = {
        .pixels = (void *)&panel, .w = 24, .h = 24, .stride = 96,
        .format = SURF_FMT_ARGB8888,
    };
    static const char *const items[] = {"AA", "BB", "CC"};
    surf_dropdown_style st = {
        .panel = &panel, .inset = 8, .font = &tfont,
        .text_color = 1, .hi_color = 2,
    };
    surf_dropdown *d = surf_dropdown_new(surf_screen(), 50, 20, 120, &st, items, 3);
    OK(d && surf_dropdown_selected(d) == 0);
    surf_dropdown_on_change(d, test_dd_cb, NULL);
    surf_tick();

    /* item_h = line_h 16 + 6 = 22; tap the box to open */
    drag(60, 30, 60, 30, 1);
    surf_tick();
    /* popup panel is on screen: hit an item row (popup at y = 20+22+2 = 44,
     * rows offset +6; row 2 spans y 88..110) */
    surf_node *row = surf_hit_test(60, 95);
    OK(row != NULL && row->type == SURF_NODE_GROUP);

    /* select the third item */
    dd_got = -1;
    drag(60, 95, 60, 95, 1);
    OK(dd_got == 2);
    OK(surf_dropdown_selected(d) == 2);
    surf_tick();
    OK(surf_hit_test(60, 95) == NULL || surf_hit_test(60, 95)->type != SURF_NODE_GROUP);

    /* open again, tap outside → scrim closes, selection unchanged */
    drag(60, 30, 60, 30, 1);
    surf_tick();
    dd_got = -1;
    drag(350, 250, 350, 250, 1);
    surf_tick();
    OK(dd_got == -1 && surf_dropdown_selected(d) == 2);
    /* popup gone: outside tap now hits nothing */
    OK(surf_hit_test(350, 250) == NULL);

    surf_dropdown_destroy(d);
}

void run_scroll_tests(void)
{
    test_scroll_compose_hit();
    test_scroll_drag_steal();
    test_axis_lock();
    test_fast_scrollview();
    test_overscroll_spring();
    test_checkbox();
    test_dropdown();
}
