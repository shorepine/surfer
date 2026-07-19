/* Regression: any path that moves framebuffer pixels behind the damage
 * system's back (hal scroll_rect — textgrid fast scroll, scrollview fast
 * scroll) must still leave the presented texture identical to the
 * framebuffer. Needs a display, so it lives in `make test-sdl`, not the
 * mock-hal `make test`. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "surfer.h"
#include "hal_sdl.h"
#include "font_mono16.h"

#define W 480
#define H 320

static int failures;

static void frame(void)
{
    surf_hal_sdl_pump();
    surf_tick();
}

static long file_diff(const char *pa, const char *pb)
{
    FILE *a = fopen(pa, "rb"), *b = fopen(pb, "rb");
    if (!a || !b)
        return -1;
    long diff = 0;
    int ca, cb;
    do {
        ca = fgetc(a);
        cb = fgetc(b);
        if (ca != cb)
            diff++;
    } while (ca != EOF && cb != EOF);
    fclose(a);
    fclose(b);
    return diff;
}

static void check(const char *what)
{
    if (!surf_hal_sdl_dump_ppm("build/spt_fb.ppm") ||
        !surf_hal_sdl_dump_screen_ppm("build/spt_screen.ppm")) {
        printf("%s: dump failed: FAIL\n", what);
        failures++;
        return;
    }
    long d = file_diff("build/spt_fb.ppm", "build/spt_screen.ppm");
    printf("%s: fb vs screen: %s (%ld bytes differ)\n", what,
           d == 0 ? "OK" : "FAIL", d);
    if (d != 0)
        failures++;
}

int main(void)
{
    const surf_hal *hal = surf_hal_sdl_init(W, H, "surfer — present coherence");
    if (!hal || !surf_init(hal, W, H, &(surf_config){.max_nodes = 64,
                                                     .bg = SURF_RGB(18, 20, 25)})) {
        fprintf(stderr, "sdl_present_test: init failed\n");
        return 1;
    }
    surf_color fg = SURF_RGB(200, 205, 215), bg = SURF_RGB(18, 20, 25);

    /* -- textgrid fast scroll (the tulip-mode console / editor path) -- */
    surf_node *probe = surf_textgrid_new(&surf_font_mono16, 1, 1, fg, bg);
    surf_point cs = surf_textgrid_cell_size(probe);
    surf_node_destroy(probe);
    int16_t cols = (int16_t)(W / cs.x), rows = (int16_t)(H / cs.y);

    surf_node *grid = surf_textgrid_new(&surf_font_mono16, cols, rows, fg, bg);
    surf_node_add(surf_screen(), grid);
    surf_textgrid_set_fast_scroll(grid, true);
    char line[128];
    for (int r = 0; r < rows; r++) {
        snprintf(line, sizeof line, "row %03d $%%&#@ abcdefghijklmnop", r);
        surf_textgrid_set_row(grid, (int16_t)r, line);
    }
    frame();
    check("textgrid full paint");
    for (int i = 0; i < 3; i++) {
        surf_textgrid_scroll(grid, 1);
        snprintf(line, sizeof line, "row %03d (scrolled in)", rows + i);
        surf_textgrid_set_row(grid, (int16_t)(rows - 1), line);
        frame();
    }
    check("textgrid fast scroll x3");
    surf_node_destroy(grid);
    frame();

    /* -- scrollview fast scroll (the gamma9001 channel-list path) -- */
    surf_node *sv = surf_scrollview_new(0, 0, W, H);
    surf_node_add(surf_screen(), sv);
    surf_scrollview_set_fast_scroll(sv, true);
    for (int i = 0; i < 40; i++)
        surf_node_add(sv, surf_rect_new(8, (int16_t)(i * 24), W - 16, 18,
                                        SURF_RGB(40 + i * 5, 80, 200 - i * 4)));
    frame();
    check("scrollview full paint");
    for (int i = 1; i <= 4; i++) {
        surf_scrollview_set_offset(sv, 0, (int16_t)(i * 13));
        frame();
    }
    check("scrollview fast scroll x4");

    surf_hal_sdl_quit();
    printf("sdl_present_test: %s\n", failures ? "FAIL" : "all OK");
    return failures ? 1 : 0;
}
