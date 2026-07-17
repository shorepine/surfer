/* M3 demo: labels (wrap, align, ellipsis) and a live textinput. Click to
 * place the caret, drag to select, type; arrows/home/end move (shift
 * extends), backspace/delete edit. Esc quits. argv[1] caps frames. */
#include <stdio.h>
#include <stdlib.h>

#include "surfer.h"
#include "hal_sdl.h"
#include "font_ui16.h"
#include "font_ui28.h"

#define W 900
#define H 420

static surf_node *input;

static void input_touch(surf_node *n, const surf_touch *t, void *user)
{
    (void)user;
    if (t->phase == SURF_TOUCH_UP)
        return;
    int16_t ax, ay;
    surf_node_abs_pos(n, &ax, &ay);
    surf_textinput_set_caret(n, surf_textinput_index_from_x(n, (int16_t)(t->x - ax)),
                             t->phase == SURF_TOUCH_MOVE);
}

int main(int argc, char **argv)
{
    long max_frames = argc > 1 ? strtol(argv[1], NULL, 10) : 0;

    const surf_hal *hal = surf_hal_sdl_init(W, H, "surfer M3 — text");
    if (!hal || !surf_init(hal, W, H, &(surf_config){.max_nodes = 64,
                                                     .bg = SURF_RGB(24, 26, 32)})) {
        fprintf(stderr, "type: init failed\n");
        return 1;
    }

    surf_node_add(surf_screen(),
                  surf_text_new(&surf_font_ui28, "surfer text", 20, 14,
                                SURF_RGB(240, 242, 248)));

    surf_node *para = surf_text_new(
        &surf_font_ui16,
        "Glyphs are baked into A8 atlases at build time by fontbake, so "
        "drawing text is the same operation as drawing everything else in "
        "surfer: small clipped blits into dirty rectangles. Greedy word wrap "
        "breaks on spaces and after hyphens - like this - and a paragraph "
        "reflows when its wrap width changes.",
        20, 64, SURF_RGB(180, 186, 198));
    surf_text_set_wrap(para, W - 40);
    surf_node_add(surf_screen(), para);

    surf_node *ell = surf_text_new(
        &surf_font_ui16,
        "This line is far too long to fit in 220 pixels so it ellipsizes",
        20, 210, SURF_RGB(240, 190, 80));
    surf_text_set_wrap(ell, 220);
    surf_text_set_ellipsis(ell, true);
    surf_node_add(surf_screen(), ell);

    int16_t line_h = surf_font_line_h(&surf_font_ui16);
    surf_node_add(surf_screen(),
                  surf_rect_new(16, (int16_t)(300 - 6), 468, (int16_t)(line_h + 12),
                                SURF_RGB(38, 42, 52)));
    input = surf_textinput_new(&surf_font_ui16, 20, 300, 460, SURF_RGB(240, 242, 248));
    surf_textinput_set_text(input, "click, drag, type");
    surf_textinput_set_focused(input, true);
    surf_node_set_on_touch(input, input_touch, NULL);
    surf_node_add(surf_screen(), input);

    long frames = 0;
    while (surf_hal_sdl_pump()) {
        surf_sdl_key k;
        while (surf_hal_sdl_poll_key(&k)) {
            switch (k.kind) {
            case SURF_KEY_TEXT:      surf_textinput_insert(input, k.utf8); break;
            case SURF_KEY_LEFT:      surf_textinput_move(input, -1, k.shift); break;
            case SURF_KEY_RIGHT:     surf_textinput_move(input, 1, k.shift); break;
            case SURF_KEY_HOME:      surf_textinput_move(input, -99999, k.shift); break;
            case SURF_KEY_END:       surf_textinput_move(input, 99999, k.shift); break;
            case SURF_KEY_BACKSPACE: surf_textinput_backspace(input); break;
            case SURF_KEY_DELETE:    surf_textinput_delete(input); break;
            }
        }
        surf_tick();
        if (max_frames && ++frames >= max_frames)
            break;
    }

    if (getenv("SURF_SHOT"))
        surf_hal_sdl_dump_ppm(getenv("SURF_SHOT"));
    surf_deinit();
    surf_hal_sdl_quit();
    return 0;
}
