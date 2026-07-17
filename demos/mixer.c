/* M1 demo: 6 knobs + 6 sliders, draggable with the mouse. Proves the feel.
 * Acceptance: holds 60 fps under continuous drag (SURF_STATS=1 for stats).
 * SURF_AUTODRAG=1 animates every control each frame — a worst-case damage
 * load for headless perf runs. argv[1] caps frame count. */
#include <stdio.h>
#include <stdlib.h>

#include "surfer.h"
#include "hal_sdl.h"
#include "widget_assets.h"

#define W 1280
#define H 720
#define N 6

/* value → readout bar; user data is the bar node */
static void bar_show(int32_t v, void *user)
{
    surf_rect_set_size(user, (int16_t)(1 + (((int64_t)v * 119) >> 16)), 8);
}

int main(int argc, char **argv)
{
    long max_frames = argc > 1 ? strtol(argv[1], NULL, 10) : 0;
    bool stats = getenv("SURF_STATS") && atoi(getenv("SURF_STATS"));
    bool autodrag = getenv("SURF_AUTODRAG") && atoi(getenv("SURF_AUTODRAG"));

    const surf_hal *hal = surf_hal_sdl_init(W, H, "surfer M1 — 6 sliders + 6 knobs");
    if (!hal) {
        fprintf(stderr, "mixer: sdl hal init failed\n");
        return 1;
    }
    surf_config cfg = {.max_nodes = 128, .bg = SURF_RGB(24, 26, 32)};
    if (!surf_init(hal, W, H, &cfg)) {
        fprintf(stderr, "mixer: surf_init failed\n");
        return 1;
    }

    surf_image knob_img = {
        .pixels = widget_knob_px, .w = WKNOB_STRIP_W, .h = WKNOB_SIZE,
        .stride = WKNOB_STRIP_W * 4, .format = SURF_FMT_ARGB8888, .opaque = false,
    };
    surf_image track_img = {
        .pixels = widget_track_px, .w = WTRACK_SIZE, .h = WTRACK_SIZE,
        .stride = WTRACK_SIZE * 4, .format = SURF_FMT_ARGB8888, .opaque = false,
    };
    surf_image cap_img = {
        .pixels = widget_cap_px, .w = WCAP_W, .h = WCAP_H,
        .stride = WCAP_W * 4, .format = SURF_FMT_ARGB8888, .opaque = false,
    };
    surf_knob_style kstyle = {
        .strip = &knob_img, .frame_w = WKNOB_SIZE, .frame_h = WKNOB_SIZE,
        .frames = WKNOB_FRAMES,
    };
    surf_slider_style sstyle = {.track = &track_img, .inset = WTRACK_INSET, .cap = &cap_img};

    surf_node_add(surf_screen(), surf_rect_new(0, 0, W, 48, SURF_RGB(38, 42, 52)));
    surf_node_add(surf_screen(), surf_rect_new(0, H - 80, W, 80, SURF_RGB(38, 42, 52)));

    surf_knob *knobs[N];
    surf_slider *sliders[N];
    surf_node *kbar[N], *sbar[N];

    for (int i = 0; i < N; i++) {
        int16_t kx = (int16_t)(158 + i * 180);
        knobs[i] = surf_knob_new(surf_screen(), kx, 100, &kstyle);
        sliders[i] = surf_slider_new(surf_screen(), (int16_t)(kx + 8), 240, 48, 360, &sstyle);

        kbar[i] = surf_rect_new(kx, 28, 1, 8, SURF_RGB(240, 190, 80));
        sbar[i] = surf_rect_new(kx, (int16_t)(H - 40), 1, 8, SURF_RGB(80, 200, 220));
        surf_node_add(surf_screen(), kbar[i]);
        surf_node_add(surf_screen(), sbar[i]);
        if (!knobs[i] || !sliders[i] || !kbar[i] || !sbar[i]) {
            fprintf(stderr, "mixer: widget setup failed\n");
            return 1;
        }
    }

    for (int i = 0; i < N; i++) {
        surf_knob_on_change(knobs[i], bar_show, kbar[i]);
        surf_slider_on_change(sliders[i], bar_show, sbar[i]);
        surf_knob_set_value(knobs[i], i * SURF_ONE / (N - 1));
        surf_slider_set_value(sliders[i], SURF_ONE - i * SURF_ONE / (N - 1));
        bar_show(surf_knob_value(knobs[i]), kbar[i]);
        bar_show(surf_slider_value(sliders[i]), sbar[i]);
    }

    long frames = 0;
    uint64_t acc = 0, worst = 0, window_start = hal->now_us();
    int window_frames = 0;
    int32_t phase = 0;

    while (surf_hal_sdl_pump()) {
        if (autodrag) {
            phase += SURF_ONE / 120;
            for (int i = 0; i < N; i++) {
                int32_t v = (phase + i * SURF_ONE / N) % SURF_ONE;
                if (v > SURF_ONE / 2)
                    v = SURF_ONE - v;
                v *= 2;
                surf_knob_set_value(knobs[i], v);
                surf_slider_set_value(sliders[i], SURF_ONE - v);
                bar_show(v, kbar[i]);
                bar_show(SURF_ONE - v, sbar[i]);
            }
        }

        uint64_t t0 = hal->now_us();
        surf_tick();
        uint64_t dt = hal->now_us() - t0;

        if (stats) {
            acc += dt;
            if (dt > worst) worst = dt;
            window_frames++;
            uint64_t now = hal->now_us();
            if (now - window_start >= 1000000) {
                printf("tick avg %.2f ms  max %.2f ms  %.1f fps\n",
                       acc / 1000.0 / window_frames, worst / 1000.0,
                       window_frames * 1e6 / (double)(now - window_start));
                acc = worst = 0;
                window_frames = 0;
                window_start = now;
            }
        }
        if (max_frames && ++frames >= max_frames)
            break;
    }

    surf_deinit();
    surf_hal_sdl_quit();
    return 0;
}
