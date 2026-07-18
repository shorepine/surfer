/* M3 tests: UTF-8, measure/wrap/kerning, label paint + damage, textinput
 * editing/caret/selection/scroll — all against a synthetic font so the
 * numbers are exact and no baked assets are needed. */
#include <string.h>

#include "mock_hal.h"

/* synthetic font: A–Z adv 10 (8×10 glyphs at x=(cp-'A')*8), space adv 5,
 * hyphen adv 6, '?' adv 10, '…' adv 12; kern pair A→V = −2.
 * ascent 12, descent −3, gap 1 → line height 16. */
static surf_glyph tglyphs[] = {
    {' ', 0, 0, 0, 0, 0, 0, 5},
    {'-', 240, 0, 4, 2, 1, -4, 6},
    {'?', 246, 0, 8, 10, 1, -10, 10},
    /* A–Z filled in by mkfont() */
    {'A', 0, 0, 8, 10, 1, -10, 10},
};
static surf_glyph tg_all[3 + 26 + 1];
static surf_kern tkerns[] = {{'A', 'V', -2}};
surf_font tfont;  /* shared with test_scroll.c's dropdown tests */

static void mkfont(void)
{
    tg_all[0] = tglyphs[0];
    tg_all[1] = tglyphs[1];
    tg_all[2] = tglyphs[2];
    for (int i = 0; i < 26; i++)
        tg_all[3 + i] = (surf_glyph){(uint32_t)('A' + i), (int16_t)(i * 8), 0,
                                     8, 10, 1, -10, 10};
    tg_all[29] = (surf_glyph){0x2026, 208, 0, 10, 4, 1, -4, 12};
    tfont = (surf_font){
        .atlas = {.pixels = (void *)tg_all, .w = 256, .h = 16, .stride = 256,
                  .format = SURF_FMT_A8},
        .ascent = 12, .descent = -3, .line_gap = 1,
        .glyphs = tg_all, .nglyphs = 30,
        .kerns = tkerns, .nkerns = 1,
    };
}

static void test_utf8(void)
{
    int32_t i = 0;
    const char *s = "A\xc3\xa9\xe2\x82\xac\xf0\x9f\x8e\x9b";  /* A é € 🎛 */
    OK(surf_utf8_next(s, &i) == 'A' && i == 1);
    OK(surf_utf8_next(s, &i) == 0xe9 && i == 3);
    OK(surf_utf8_next(s, &i) == 0x20ac && i == 6);
    OK(surf_utf8_next(s, &i) == 0x1f39b && i == 10);
    OK(surf_utf8_next(s, &i) == 0);
    OK(surf_utf8_prev(s, 10) == 6);
    OK(surf_utf8_prev(s, 6) == 3);
    OK(surf_utf8_prev(s, 1) == 0);
    OK(surf_utf8_prev(s, 0) == 0);
}

static bool pt_eq(surf_point p, int16_t x, int16_t y) { return p.x == x && p.y == y; }

static void test_measure(void)
{
    OK(surf_font_line_h(&tfont) == 16);
    OK(pt_eq(surf_text_measure(&tfont, "AB", 0), 20, 16));
    OK(pt_eq(surf_text_measure(&tfont, "AV", 0), 18, 16));  /* kern −2 */
    OK(pt_eq(surf_text_measure(&tfont, "AA BB", 0), 45, 16));
    OK(pt_eq(surf_text_measure(&tfont, "", 0), 0, 16));

    /* greedy wrap on space: trailing space never counts */
    OK(pt_eq(surf_text_measure(&tfont, "AA BB CC", 25), 20, 48));
    /* break after hyphen, hyphen stays on the line */
    OK(pt_eq(surf_text_measure(&tfont, "AA-BB", 30), 26, 32));
    /* a word wider than the box hard-breaks mid-word */
    OK(pt_eq(surf_text_measure(&tfont, "AAAA", 25), 20, 32));
    /* explicit newlines always break */
    OK(pt_eq(surf_text_measure(&tfont, "A\nB", 0), 10, 32));
    /* missing glyph falls back to '?' */
    OK(pt_eq(surf_text_measure(&tfont, "A~", 0), 20, 16));
}

static int count_op(char op)
{
    int c = 0;
    for (int i = 0; i < nops; i++)
        if (ops[i].op == op)
            c++;
    return c;
}

static void test_label(void)
{
    fresh(200, 100, 32);

    surf_node *t = surf_text_new(&tfont, "AB", 0, 0, SURF_RGB(255, 255, 255));
    OK(t && t->w == 20 && t->h == 16);
    surf_node_add(surf_screen(), t);
    surf_tick();
    /* bg fill + 2 glyph blends + present */
    OK(count_op('A') == 2);
    OK(ops[1].op == 'A' && ops[1].img == &t->u.text.img);
    OK(ops[1].dst.x == 1 && ops[1].dst.y == 2);   /* xoff 1, 12 − 10 */
    OK(ops[1].src.x == 0 && ops[1].src.w == 8);   /* 'A' cell */
    OK(ops[2].dst.x == 11 && ops[2].src.x == 8);  /* 'B' cell */

    /* centered in a wrap box */
    surf_text_set_wrap(t, 60);
    surf_text_set_align(t, SURF_ALIGN_CENTER);
    surf_tick();
    nops = 0;
    surf_damage_subtree(t);
    surf_tick();
    OK(ops[1].op == 'A' && ops[1].dst.x == 21);  /* (60−20)/2 + xoff */

    /* set_text damages old and new bounds */
    surf_text_set_wrap(t, 0);
    surf_text_set_align(t, SURF_ALIGN_LEFT);
    surf_tick();
    surf_text_set(t, "ABCD");
    OK(surf_g.dirty.n >= 1 && surf_rect_covers(surf_g.dirty.r[0], (surf_rect){0, 0, 40, 16}));
    surf_tick();

    /* ellipsize: AAAA @ 35 → A A … */
    surf_text_set(t, "AAAA");
    surf_text_set_wrap(t, 35);
    surf_text_set_ellipsis(t, true);
    OK(t->w == 35 && t->h == 16);
    nops = 0;
    surf_tick();
    OK(count_op('A') == 3);
    bool ell = false;
    for (int i = 0; i < nops; i++)
        if (ops[i].op == 'A' && ops[i].src.x == 208)
            ell = true;
    OK(ell);

    surf_node_destroy(t);
}

static void test_textinput(void)
{
    fresh(200, 100, 32);

    surf_node *n = surf_textinput_new(&tfont, 10, 10, 50, SURF_RGB(255, 255, 255));
    OK(n && n->w == 50 && n->h == 16);
    surf_node_add(surf_screen(), n);

    surf_textinput_set_text(n, "ABC");
    OK(strcmp(surf_textinput_text(n), "ABC") == 0);
    OK(surf_textinput_caret(n) == 3);

    surf_textinput_insert(n, "D");
    OK(strcmp(surf_textinput_text(n), "ABCD") == 0 && surf_textinput_caret(n) == 4);

    surf_textinput_move(n, -2, false);
    OK(surf_textinput_caret(n) == 2);
    surf_textinput_backspace(n);
    OK(strcmp(surf_textinput_text(n), "ACD") == 0 && surf_textinput_caret(n) == 1);
    surf_textinput_delete(n);
    OK(strcmp(surf_textinput_text(n), "AD") == 0 && surf_textinput_caret(n) == 1);

    /* selection replace */
    surf_textinput_set_caret(n, 0, false);
    surf_textinput_set_caret(n, 2, true);
    surf_textinput_insert(n, "Z");
    OK(strcmp(surf_textinput_text(n), "Z") == 0 && surf_textinput_caret(n) == 1);

    /* caret from x: nearest boundary, scroll-aware */
    surf_textinput_set_text(n, "AB");
    OK(surf_textinput_index_from_x(n, 4) == 0);
    OK(surf_textinput_index_from_x(n, 6) == 1);
    OK(surf_textinput_index_from_x(n, 100) == 2);

    /* scroll-into-view: caret at the end of 100px of text in a 50px box */
    surf_textinput_set_text(n, "AAAAAAAAAA");
    OK(n->u.input.scroll_x == 100 - (50 - 2 - 2));
    surf_textinput_move(n, -9999, false);
    OK(n->u.input.scroll_x == 0);

    /* focused caret paints as a 2px fill in the text color */
    surf_textinput_set_text(n, "AB");
    surf_textinput_set_focused(n, true);
    surf_tick();
    nops = 0;
    surf_damage_subtree(n);
    surf_tick();
    bool caret = false;
    for (int i = 0; i < nops; i++)
        if (ops[i].op == 'F' && ops[i].r.w == 2 && ops[i].r.h == 16 &&
            ops[i].r.x == 10 + 20)
            caret = true;
    OK(caret);

    /* selection highlight fills behind the glyphs */
    surf_textinput_set_caret(n, 0, false);
    surf_textinput_set_caret(n, 2, true);
    surf_tick();
    nops = 0;
    surf_damage_subtree(n);
    surf_tick();
    bool sel = false;
    for (int i = 0; i < nops; i++)
        if (ops[i].op == 'F' && ops[i].r.w == 20 && ops[i].r.x == 10 && ops[i].r.h == 16)
            sel = true;
    OK(sel);

    surf_node_destroy(n);
}

void run_text_tests(void)
{
    mkfont();
    test_utf8();
    test_measure();
    test_label();
    test_textinput();
}
