/* M4 tests: scrollview compose/hit/damage through the content offset,
 * gesture steal vs grab, momentum decay, edge spring-back, checkbox,
 * dropdown popup lifecycle. Touches go through the real dispatch path. */
#include <string.h>

#include "mock_hal.h"

static void drag(int16_t x0, int16_t y0, int16_t x1, int16_t y1, int steps)
{
    mock_push_touch((surf_touch){x0, y0, SURF_TOUCH_DOWN, 0});
    surf_tick();
    for (int i = 1; i <= steps; i++) {
        mock_push_touch((surf_touch){
            (int16_t)(x0 + (x1 - x0) * i / steps),
            (int16_t)(y0 + (y1 - y0) * i / steps), SURF_TOUCH_MOVE, 0});
        surf_tick();
    }
    mock_push_touch((surf_touch){x1, y1, SURF_TOUCH_UP, 0});
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

/* The scrollview's copy of the hal-shift rule: a viewport inside a
 * hidden group paints nothing, so scroll_rect must not run over pixels
 * it does not own. See test_layer.c for the full reasoning. */
static void test_scroll_hidden_ancestor_never_shifts(void)
{
    fresh(200, 200, 64);
    surf_node *app = surf_group_new(0, 0);
    surf_node_add(surf_screen(), app);
    surf_node *sv = surf_scrollview_new(10, 20, 100, 100);
    surf_node_add(app, sv);
    surf_node_add(sv, surf_rect_new(0, 0, 80, 300, 1));
    surf_scrollview_set_fast_scroll(sv, true);
    surf_tick();

    nops = 0;
    surf_scrollview_set_offset(sv, 0, 30);      /* control: visible shifts */
    OK(nops == 1 && ops[0].op == 'S');
    surf_tick();

    surf_node_set_hidden(app, true);
    surf_tick();

    nops = 0;
    surf_scrollview_set_offset(sv, 0, 60);
    OK(nops == 0);
    surf_node_destroy(app);
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

    /* two shifts landing in ONE tick: the first exposure strip must ride
     * the second shift or a stale band tears into view */
    surf_scrollview_set_offset(sv, 0, 0);
    surf_tick();
    surf_inject_touch(&(surf_touch){50, 100, SURF_TOUCH_DOWN, 0});
    nops = 0;
    surf_inject_touch(&(surf_touch){50, 90, SURF_TOUCH_MOVE, 0});  /* +10 px */
    surf_inject_touch(&(surf_touch){50, 82, SURF_TOUCH_MOVE, 0});  /* +8 px */
    {
        int shifts = 0;
        for (int i = 0; i < nops; i++)
            if (ops[i].op == 'S')
                shifts++;
        OK(shifts == 2);
        /* the union of both exposures — the bottom 18px — must be dirty
         * before compose runs, or the moved first strip never repaints */
        surf_rect need = {10, (int16_t)(20 + 100 - 18), 100, 18};
        bool covered = false;
        for (int i = 0; i < surf_g.dirty.n && !covered; i++)
            covered = surf_rect_covers(surf_g.dirty.r[i], need);
        OK(covered);
    }
    surf_inject_touch(&(surf_touch){50, 82, SURF_TOUCH_UP, 0});
    surf_tick();
    while (surf_g.nscrollers > 0)
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


/* A list longer than the screen SCROLLS inside a popup that fits, and a
 * drag through it scrolls rather than picking whatever it crossed. It
 * used to draw full length straight down, off the bottom of the screen. */
/* drag() with the mock's op log cleared every tick: a scrolling list of
 * thirty rows records more ops than the log holds */
static void tick0(void) { nops = 0; surf_tick(); }
static void drag0(int16_t x0, int16_t y0, int16_t x1, int16_t y1, int steps)
{
    mock_push_touch((surf_touch){x0, y0, SURF_TOUCH_DOWN, 0});
    tick0();
    for (int i = 1; i <= steps; i++) {
        mock_push_touch((surf_touch){
            (int16_t)(x0 + (x1 - x0) * i / steps),
            (int16_t)(y0 + (y1 - y0) * i / steps), SURF_TOUCH_MOVE, 0});
        tick0();
    }
    mock_push_touch((surf_touch){x1, y1, SURF_TOUCH_UP, 0});
    tick0();
}

static void test_dropdown_long(void)
{
    fresh(400, 300, 256);
    static surf_image panel = {
        .pixels = (void *)&panel, .w = 24, .h = 24, .stride = 96,
        .format = SURF_FMT_ARGB8888,
    };
    static const char *items[30];
    static char names[30][4];
    for (int i = 0; i < 30; i++) {
        names[i][0] = 'A'; names[i][1] = (char)('A' + i % 26); names[i][2] = 0;
        items[i] = names[i];
    }
    surf_dropdown_style st = {
        .panel = &panel, .inset = 8, .font = &tfont,
        .text_color = 1, .hi_color = 2,
    };
    OK(surf_node_size(surf_screen()).y == 300);

    /* near the top: opens below, clamped to the screen (item_h 22, popup
     * at y 44, rows from 50 down to 292, 30 x 22 = 660 of content) */
    surf_dropdown *d = surf_dropdown_new(surf_screen(), 50, 20, 120, &st, items, 30);
    surf_dropdown_on_change(d, test_dd_cb, NULL);
    tick0();
    drag0(60, 30, 60, 30, 1);
    tick0();
    OK(surf_hit_test(60, 285) != NULL);               /* rows reach the bottom */
    dd_got = -1;
    drag0(60, 285, 60, 285, 1);                       /* ...and are live there */
    OK(dd_got == 10);                                 /* (285 - 50) / 22 */
    drag0(60, 30, 60, 30, 1);                         /* open it again */
    tick0();

    /* a drag scrolls and picks nothing */
    dd_got = -1;
    drag0(60, 250, 60, 150, 10);
    OK(dd_got == -1);
    for (int i = 0; i < 120; i++) tick0();        /* let momentum settle */

    /* ...and a tap after it picks the row that is THERE now, not row 2 */
    dd_got = -1;
    drag0(60, 100, 60, 100, 1);
    OK(dd_got > 2);
    surf_dropdown_destroy(d);

    /* near the bottom: there is more room above, so it opens UP */
    fresh(400, 300, 256);
    d = surf_dropdown_new(surf_screen(), 50, 260, 120, &st, items, 30);
    surf_dropdown_on_change(d, test_dd_cb, NULL);
    tick0();
    drag0(60, 270, 60, 270, 1);
    tick0();
    dd_got = -1;
    drag0(60, 30, 60, 30, 1);                          /* a row up top */
    OK(dd_got >= 0);
    surf_dropdown_destroy(d);
}

/* A wheel scrolls the scrollview under it, and what no scrollview takes
 * is QUEUED for the application. The second half is what makes a wheel
 * mean something other than scrolling — zooming a picture, stepping a
 * value — and the first half is what keeps a dialog's own list working
 * while it does. */
static void test_wheel_queue(void)
{
    fresh(200, 200, 64);
    surf_node *sv = surf_scrollview_new(10, 10, 100, 100);
    surf_node_add(surf_screen(), sv);
    surf_node_add(sv, surf_rect_new(0, 0, 40, 300, 1));
    surf_tick();

    surf_wheel w;
    OK(!surf_wheel_poll(&w));               /* nothing pending to begin with */

    surf_input_wheel(50, 50, 0, 30);        /* over the list: it scrolls */
    OK(surf_scrollview_offset(sv).y == 30);
    OK(!surf_wheel_poll(&w));               /* ...and the app hears nothing */

    surf_input_wheel(160, 160, 0, 30);      /* over bare screen: the app's */
    OK(surf_scrollview_offset(sv).y == 30);
    OK(surf_wheel_poll(&w));
    OK(w.x == 160 && w.y == 160 && w.dx == 0 && w.dy == 30);
    OK(!surf_wheel_poll(&w));               /* drained, like keys() */

    /* A scrollview that cannot move is not a consumer: the wheel falls
     * through to the app rather than being swallowed by a dead list. */
    surf_node *flat = surf_scrollview_new(120, 10, 40, 40);
    surf_node_add(surf_screen(), flat);
    surf_node_add(flat, surf_rect_new(0, 0, 10, 10, 2));
    surf_tick();
    surf_input_wheel(130, 20, 0, 30);
    OK(surf_wheel_poll(&w) && w.x == 130);

    /* Overflow drops rather than wrapping — a wheel nobody drains is a
     * wheel nobody wants. */
    for (int i = 0; i < 200; i++)
        surf_input_wheel(160, 160, 0, 1);
    int n = 0;
    while (surf_wheel_poll(&w))
        n++;
    OK(n > 0 && n < 200);
}

void run_scroll_tests(void)
{
    test_wheel_queue();
    test_scroll_compose_hit();
    test_scroll_drag_steal();
    test_axis_lock();
    test_fast_scrollview();
    test_scroll_hidden_ancestor_never_shifts();
    test_overscroll_spring();
    test_checkbox();
    test_dropdown();
    test_dropdown_long();
}
