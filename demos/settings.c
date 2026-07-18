/* M4 demo: a scrollable settings panel. Drag anywhere in the list to
 * scroll (checkbox taps lose to the 8px steal threshold; slider/knob
 * drags never do), flick for momentum, overscroll springs back. The
 * dropdown's popup overlays everything from the screen root. Esc quits.
 * argv[1] caps frames; SURF_SHOT dumps the framebuffer. */
#include <stdio.h>
#include <stdlib.h>

#include "surfer.h"
#include "hal_sdl.h"
#include "widget_assets.h"
#include "font_ui16.h"
#include "font_ui28.h"

#define W 900
#define H 520
#define ROWS 10

static surf_node *status;

static void on_check(int32_t v, void *user)
{
    (void)user;
    surf_text_set(status, v ? "checked" : "unchecked");
}

static void on_wave(int32_t idx, void *user)
{
    const char *const *items = user;
    surf_text_set(status, items[idx]);
}

int main(int argc, char **argv)
{
    long max_frames = argc > 1 ? strtol(argv[1], NULL, 10) : 0;

    const surf_hal *hal = surf_hal_sdl_init(W, H, "surfer M4 — scrollview");
    if (!hal || !surf_init(hal, W, H, &(surf_config){.max_nodes = 256,
                                                     .bg = SURF_RGB(24, 26, 32)})) {
        fprintf(stderr, "settings: init failed\n");
        return 1;
    }

    surf_image knob_img = {
        .pixels = (void *)widget_knob_px, .w = WKNOB_STRIP_W, .h = WKNOB_SIZE,
        .stride = WKNOB_STRIP_W * 4, .format = SURF_FMT_ARGB8888,
    };
    surf_image track_img = {
        .pixels = (void *)widget_trackfull_px, .w = WTRACKFULL_W, .h = WTRACKFULL_H,
        .stride = WTRACKFULL_W * 4, .format = SURF_FMT_ARGB8888,
    };
    surf_image cap_img = {
        .pixels = (void *)widget_cap_px, .w = WCAP_W, .h = WCAP_H,
        .stride = WCAP_W * 4, .format = SURF_FMT_ARGB8888,
    };
    surf_image check_img = {
        .pixels = (void *)widget_check_px, .w = WCHECK_SIZE * 2, .h = WCHECK_SIZE,
        .stride = WCHECK_SIZE * 2 * 4, .format = SURF_FMT_ARGB8888,
    };
    surf_image panel_img = {
        .pixels = (void *)widget_panel_px, .w = WPANEL_SIZE, .h = WPANEL_SIZE,
        .stride = WPANEL_SIZE * 4, .format = SURF_FMT_ARGB8888,
    };
    surf_image arrow_img = {
        .pixels = (void *)widget_arrow_px, .w = WARROW_W * 2, .h = WARROW_H,
        .stride = WARROW_W * 2 * 4, .format = SURF_FMT_ARGB8888,
    };

    surf_checkbox_style cstyle = {.strip = &check_img, .frame_w = WCHECK_SIZE,
                                  .frame_h = WCHECK_SIZE};
    surf_slider_style sstyle = {.track = &track_img, .inset = WTRACK_INSET,
                                .cap = &cap_img};
    surf_knob_style kstyle = {.strip = &knob_img, .frame_w = WKNOB_SIZE,
                              .frame_h = WKNOB_SIZE, .frames = WKNOB_FRAMES};
    surf_dropdown_style dstyle = {
        .panel = &panel_img, .inset = WPANEL_INSET, .font = &surf_font_ui16,
        .text_color = SURF_RGB(240, 242, 248), .hi_color = SURF_RGB(60, 90, 140),
        .arrow = &arrow_img, .arrow_w = WARROW_W, .arrow_h = WARROW_H,
    };

    surf_node_add(surf_screen(),
                  surf_text_new(&surf_font_ui28, "surfer settings", 20, 12,
                                SURF_RGB(240, 242, 248)));
    status = surf_text_new(&surf_font_ui16, "drag the list; flick it", 640, 26,
                           SURF_RGB(180, 186, 198));
    surf_node_add(surf_screen(), status);

    /* the scrolling panel: rows of checkbox+label, then sliders and a knob */
    surf_node *sv = surf_scrollview_new(20, 64, 430, (int16_t)(H - 84));
    surf_node_add(surf_screen(), sv);

    static char rowname[ROWS][24];
    int16_t y = 4;
    for (int i = 0; i < ROWS; i++) {
        snprintf(rowname[i], sizeof rowname[i], "option row %d", i + 1);
        surf_checkbox *cb = surf_checkbox_new(sv, 4, y, &cstyle);
        surf_checkbox_on_change(cb, on_check, NULL);
        if (i % 3 == 0)
            surf_checkbox_set_checked(cb, true);
        surf_node_add(sv, surf_text_new(&surf_font_ui16, rowname[i], 44,
                                        (int16_t)(y + 5), SURF_RGB(200, 205, 215)));
        y = (int16_t)(y + 40);
    }
    for (int i = 0; i < 2; i++) {
        surf_slider *sl = surf_slider_new(sv, (int16_t)(20 + i * 90), y,
                                          WTRACKFULL_W, WTRACKFULL_H, &sstyle);
        surf_slider_set_value(sl, (i + 1) * SURF_ONE / 3);
    }
    surf_knob *kn = surf_knob_new(sv, 240, (int16_t)(y + 40), &kstyle);
    surf_knob_set_value(kn, SURF_ONE / 2);
    surf_node_add(sv, surf_text_new(&surf_font_ui16,
                                    "sliders and knobs keep their drags -\n"
                                    "scroll by dragging anywhere else",
                                    240, (int16_t)(y + 130), SURF_RGB(140, 146, 158)));
    y = (int16_t)(y + WTRACKFULL_H + 20);

    /* dropdown outside the list */
    static const char *const waves[] = {"sine", "sawtooth", "square", "triangle",
                                        "noise", "wavetable"};
    surf_dropdown *dd = surf_dropdown_new(surf_screen(), 500, 80, 180, &dstyle,
                                          waves, 6);
    surf_dropdown_on_change(dd, on_wave, (void *)waves);
    surf_node_add(surf_screen(),
                  surf_text_new(&surf_font_ui16, "oscillator", 500, 60,
                                SURF_RGB(140, 146, 158)));

    long frames = 0;
    while (surf_hal_sdl_pump()) {
        surf_sdl_key k;
        while (surf_hal_sdl_poll_key(&k)) {}
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
