/* M5-pre demo: the textgrid fast text path vs the naive per-glyph path,
 * shaped like a code editor. Up/Down scroll a line, PgUp/PgDn a page.
 *
 *   SURF_NAIVE=1       use one label node per line instead of the grid
 *   SURF_AUTOSCROLL=1  scroll one line per frame (worst case) + stats
 *   SURF_AUTOSCROLL=2  page per frame
 *
 * Each stats line also prints the glyph-op count a scroll costs and what
 * that would take on the ESP32-P4's PPA at the measured ~85µs/op — the
 * arithmetic that motivates the grid (DESIGN.md §5.6). */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "surfer.h"
#include "hal_sdl.h"
#include "font_mono16.h"

static char *dup_str(const char *s)
{
    char *p = malloc(strlen(s) + 1);
    if (p)
        strcpy(p, s);
    return p;
}

#define W 1024
#define H 600
#define MAXLINES 4096

static char *lines[MAXLINES];
static int nlines;

static void load_source(const char *path)
{
    FILE *f = fopen(path, "r");
    char buf[512];
    while (f && nlines < MAXLINES && fgets(buf, sizeof buf, f)) {
        buf[strcspn(buf, "\n")] = 0;
        /* crude tab expansion keeps columns honest */
        char ex[512];
        int o = 0;
        for (int i = 0; buf[i] && o < 500; i++) {
            if (buf[i] == '\t')
                do ex[o++] = ' '; while (o % 4);
            else
                ex[o++] = buf[i];
        }
        ex[o] = 0;
        lines[nlines++] = dup_str(ex);
    }
    if (f)
        fclose(f);
    while (nlines < 200) {  /* fallback / padding */
        char tmp[64];
        snprintf(tmp, sizeof tmp, "    line %d of generated filler text;", nlines);
        lines[nlines] = dup_str(tmp);
        nlines++;
    }
}

int main(int argc, char **argv)
{
    long max_frames = argc > 1 ? strtol(argv[1], NULL, 10) : 0;
    bool stats = getenv("SURF_STATS") && atoi(getenv("SURF_STATS"));
    int autoscroll = getenv("SURF_AUTOSCROLL") ? atoi(getenv("SURF_AUTOSCROLL")) : 0;
    bool naive = getenv("SURF_NAIVE") && atoi(getenv("SURF_NAIVE"));

    load_source("demos/editor.c");

    const surf_hal *hal = surf_hal_sdl_init(W, H, "surfer — editor scroll test");
    if (!hal || !surf_init(hal, W, H, &(surf_config){.max_nodes = 128,
                                                     .bg = SURF_RGB(18, 20, 25)})) {
        fprintf(stderr, "editor: init failed\n");
        return 1;
    }

    surf_node *grid = NULL;
    surf_node *labels[64];
    int16_t cols, rows;
    {
        surf_node *probe = surf_textgrid_new(&surf_font_mono16, 1, 1, 0, 0);
        surf_point cs = surf_textgrid_cell_size(probe);
        surf_node_destroy(probe);
        cols = (int16_t)(W / cs.x);
        rows = (int16_t)(H / cs.y);
    }
    int16_t line_h = surf_font_line_h(&surf_font_mono16);
    surf_color fg = SURF_RGB(200, 205, 215), bg = SURF_RGB(18, 20, 25);

    int top = 0;
    long glyphs_per_scroll = 0;

    if (!naive) {
        grid = surf_textgrid_new(&surf_font_mono16, cols, rows, fg, bg);
        surf_node_add(surf_screen(), grid);
    } else {
        for (int r = 0; r < rows && r < 64; r++) {
            labels[r] = surf_text_new(&surf_font_mono16, "", 0,
                                      (int16_t)(r * line_h), fg);
            surf_node_add(surf_screen(), labels[r]);
        }
    }

#define REFRESH() do {                                                        \
        glyphs_per_scroll = 0;                                                \
        for (int r = 0; r < rows; r++) {                                      \
            const char *s = (top + r < nlines) ? lines[top + r] : "";         \
            if (naive)                                                        \
                surf_text_set(labels[r], s);                                  \
            else                                                              \
                surf_textgrid_set_row(grid, (int16_t)r, s);                   \
            for (const char *p = s; *p; p++)                                  \
                if (*p != ' ')                                                \
                    glyphs_per_scroll++;                                      \
        }                                                                     \
        if (!naive)                                                           \
            glyphs_per_scroll = (long)cols * rows; /* every cell repaints */  \
    } while (0)

    REFRESH();

    long frames = 0;
    uint64_t acc = 0, worst = 0, win_start = hal->now_us();
    int win_frames = 0;

    while (surf_hal_sdl_pump()) {
        surf_sdl_key k;
        while (surf_hal_sdl_poll_key(&k)) {
            int step = 0;
            if (k.kind == SURF_KEY_DOWN) step = 1;
            if (k.kind == SURF_KEY_UP) step = -1;
            if (k.kind == SURF_KEY_PGDN) step = rows;
            if (k.kind == SURF_KEY_PGUP) step = -rows;
            if (step) {
                top += step;
                if (top < 0) top = 0;
                if (top > nlines - rows) top = nlines - rows;
                REFRESH();
            }
        }
        if (autoscroll) {
            top += autoscroll == 2 ? rows : 1;
            if (top > nlines - rows)
                top = 0;
            REFRESH();
        }

        uint64_t t0 = hal->now_us();
        surf_tick();
        uint64_t dt = hal->now_us() - t0;

        if (stats) {
            acc += dt;
            if (dt > worst) worst = dt;
            win_frames++;
            uint64_t now = hal->now_us();
            if (now - win_start >= 1000000) {
                printf("[%s] tick avg %.2f ms  max %.2f ms  %.1f fps | "
                       "%ld ops/scroll → P4 PPA @85us/op ≈ %.0f ms\n",
                       naive ? "naive" : "grid", acc / 1000.0 / win_frames,
                       worst / 1000.0, win_frames * 1e6 / (double)(now - win_start),
                       glyphs_per_scroll, glyphs_per_scroll * 0.085);
                acc = worst = 0;
                win_frames = 0;
                win_start = now;
            }
        }
        if (max_frames && ++frames >= max_frames)
            break;
    }

    if (getenv("SURF_SHOT"))
        surf_hal_sdl_dump_ppm(getenv("SURF_SHOT"));
    surf_deinit();
    surf_hal_sdl_quit();
    return 0;
}
