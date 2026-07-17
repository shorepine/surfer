/* M2: the whole bet, measured. Phase A benchmarks raw bandwidth (CPU and
 * PPA) at 1080p buffer sizes plus per-op PPA overhead — the number that
 * decides whether a frame of many small blits is viable. Phase B brings
 * up the EK79007 DSI panel + GT911 touch via the board BSP and runs the
 * M1 mixer scene: 6 knobs + 6 sliders, dragged by finger or by autodrag
 * (whenever untouched for 2 s), with per-second frame stats on serial. */
#include <inttypes.h>
#include <stdio.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "bsp/esp-bsp.h"
#include "bsp/touch.h"
#include "driver/ppa.h"
#include "esp_cache.h"
#include "esp_heap_caps.h"
#include "esp_lcd_mipi_dsi.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_touch.h"
#include "esp_timer.h"

#include "surfer.h"
#include "hal_p4.h"
#include "widget_assets.h"
#include "font_ui16.h"
#include "font_ui28.h"

static const char *knob_names[6] = {"cutoff", "res", "env", "lfo", "mix", "vol"};

#define BENCH_W 1920
#define BENCH_H 1080
#define LCD_W   1024
#define LCD_H   600
#define N       6

/* ---- phase A: bandwidth + op overhead ---- */

static double mbps(size_t bytes, int reps, int64_t us)
{
    return (double)bytes * reps / us;  /* bytes/us == MB/s */
}

static void bench(void)
{
    printf("\n== surfer M2 bench: 1080p RGB565 = %.2f MB/frame ==\n",
           BENCH_W * BENCH_H * 2 / 1e6);

    size_t sz565 = (size_t)BENCH_W * BENCH_H * 2;
    size_t sz8888 = (size_t)BENCH_W * BENCH_H * 4;
    uint8_t *a = heap_caps_aligned_alloc(128, sz565, MALLOC_CAP_SPIRAM);
    uint8_t *b = heap_caps_aligned_alloc(128, sz565, MALLOC_CAP_SPIRAM);
    uint8_t *f = heap_caps_aligned_alloc(128, sz8888, MALLOC_CAP_SPIRAM);
    if (!a || !b || !f) {
        printf("bench: PSRAM alloc failed (a=%p b=%p f=%p)\n", a, b, f);
        return;
    }
    memset(f, 0x80, sz8888);
    esp_cache_msync(f, sz8888, ESP_CACHE_MSYNC_FLAG_DIR_C2M);
    memset(a, 0x11, sz565);
    esp_cache_msync(a, sz565, ESP_CACHE_MSYNC_FLAG_DIR_C2M);

    int64_t t0;
    const int R = 10;

    t0 = esp_timer_get_time();
    for (int i = 0; i < R; i++)
        memset(a, i, sz565);
    int64_t t_memset = esp_timer_get_time() - t0;
    printf("cpu memset  1080p:  %6.2f ms/frame  %7.1f MB/s\n",
           t_memset / 1000.0 / R, mbps(sz565, R, t_memset));

    t0 = esp_timer_get_time();
    for (int i = 0; i < R; i++)
        memcpy(b, a, sz565);
    int64_t t_memcpy = esp_timer_get_time() - t0;
    printf("cpu memcpy  1080p:  %6.2f ms/frame  %7.1f MB/s (r+w)\n",
           t_memcpy / 1000.0 / R, mbps(sz565 * 2, R, t_memcpy));

    ppa_client_handle_t fill_cl, srm_cl, blend_cl;
    ppa_client_config_t cc = {.oper_type = PPA_OPERATION_FILL};
    ESP_ERROR_CHECK(ppa_register_client(&cc, &fill_cl));
    cc.oper_type = PPA_OPERATION_SRM;
    ESP_ERROR_CHECK(ppa_register_client(&cc, &srm_cl));
    cc.oper_type = PPA_OPERATION_BLEND;
    ESP_ERROR_CHECK(ppa_register_client(&cc, &blend_cl));

    ppa_fill_oper_config_t fill = {
        .out = {.buffer = a, .buffer_size = sz565, .pic_w = BENCH_W, .pic_h = BENCH_H,
                .fill_cm = PPA_FILL_COLOR_MODE_RGB565},
        .fill_block_w = BENCH_W, .fill_block_h = BENCH_H,
        .fill_argb_color = {.a = 0xff, .r = 0x30, .g = 0x60, .b = 0x90},
        .mode = PPA_TRANS_MODE_BLOCKING,
    };
    t0 = esp_timer_get_time();
    for (int i = 0; i < R; i++)
        ESP_ERROR_CHECK(ppa_do_fill(fill_cl, &fill));
    int64_t t_fill = esp_timer_get_time() - t0;
    printf("ppa fill    1080p:  %6.2f ms/frame  %7.1f MB/s\n",
           t_fill / 1000.0 / R, mbps(sz565, R, t_fill));

    ppa_srm_oper_config_t srm = {
        .in = {.buffer = a, .pic_w = BENCH_W, .pic_h = BENCH_H,
               .block_w = BENCH_W, .block_h = BENCH_H,
               .srm_cm = PPA_SRM_COLOR_MODE_RGB565},
        .out = {.buffer = b, .buffer_size = sz565, .pic_w = BENCH_W, .pic_h = BENCH_H,
                .srm_cm = PPA_SRM_COLOR_MODE_RGB565},
        .rotation_angle = PPA_SRM_ROTATION_ANGLE_0,
        .scale_x = 1.0f, .scale_y = 1.0f,
        .mode = PPA_TRANS_MODE_BLOCKING,
    };
    t0 = esp_timer_get_time();
    for (int i = 0; i < R; i++)
        ESP_ERROR_CHECK(ppa_do_scale_rotate_mirror(srm_cl, &srm));
    int64_t t_srm = esp_timer_get_time() - t0;
    printf("ppa copy    1080p:  %6.2f ms/frame  %7.1f MB/s (r+w)\n",
           t_srm / 1000.0 / R, mbps(sz565 * 2, R, t_srm));

    ppa_blend_oper_config_t bl = {
        .in_bg = {.buffer = a, .pic_w = BENCH_W, .pic_h = BENCH_H,
                  .block_w = BENCH_W, .block_h = BENCH_H,
                  .blend_cm = PPA_BLEND_COLOR_MODE_RGB565},
        .in_fg = {.buffer = f, .pic_w = BENCH_W, .pic_h = BENCH_H,
                  .block_w = BENCH_W, .block_h = BENCH_H,
                  .blend_cm = PPA_BLEND_COLOR_MODE_ARGB8888},
        .out = {.buffer = b, .buffer_size = sz565, .pic_w = BENCH_W, .pic_h = BENCH_H,
                .blend_cm = PPA_BLEND_COLOR_MODE_RGB565},
        .bg_alpha_update_mode = PPA_ALPHA_FIX_VALUE,
        .bg_alpha_fix_val = 0xff,
        .mode = PPA_TRANS_MODE_BLOCKING,
    };
    t0 = esp_timer_get_time();
    for (int i = 0; i < R; i++)
        ESP_ERROR_CHECK(ppa_do_blend(blend_cl, &bl));
    int64_t t_blend = esp_timer_get_time() - t0;
    printf("ppa blend   1080p:  %6.2f ms/frame  %7.1f MB/s (2r+w)\n",
           t_blend / 1000.0 / R, mbps(sz565 * 2 + sz8888, R, t_blend));

    /* per-op overhead at widget size — the many-small-blits question */
    const int OPS = 1000;
    fill.fill_block_w = 64;
    fill.fill_block_h = 64;
    t0 = esp_timer_get_time();
    for (int i = 0; i < OPS; i++)
        ppa_do_fill(fill_cl, &fill);
    printf("ppa fill    64x64:  %6.1f us/op\n",
           (esp_timer_get_time() - t0) / (double)OPS);

    srm.in.block_w = srm.in.block_h = 64;
    t0 = esp_timer_get_time();
    for (int i = 0; i < OPS; i++)
        ppa_do_scale_rotate_mirror(srm_cl, &srm);
    printf("ppa copy    64x64:  %6.1f us/op\n",
           (esp_timer_get_time() - t0) / (double)OPS);

    bl.in_bg.block_w = bl.in_bg.block_h = 64;
    bl.in_fg.block_w = bl.in_fg.block_h = 64;
    t0 = esp_timer_get_time();
    for (int i = 0; i < OPS; i++)
        ppa_do_blend(blend_cl, &bl);
    double us_blend = (esp_timer_get_time() - t0) / (double)OPS;
    printf("ppa blend   64x64:  %6.1f us/op\n", us_blend);
    printf("→ a 24-blend mixer frame ≈ %.2f ms of PPA time\n\n", us_blend * 24 / 1000.0);

    ppa_unregister_client(fill_cl);
    ppa_unregister_client(srm_cl);
    ppa_unregister_client(blend_cl);
    heap_caps_free(a);
    heap_caps_free(b);
    heap_caps_free(f);
}

/* ---- phase B: panel + touch + mixer scene ---- */

static esp_lcd_touch_handle_t s_touch;
static int64_t s_last_touch_us;

static bool touch_poll(int16_t *x, int16_t *y)
{
    if (!s_touch)
        return false;
    esp_lcd_touch_read_data(s_touch);
    uint16_t tx, ty;
    uint8_t cnt = 0;
    if (!esp_lcd_touch_get_coordinates(s_touch, &tx, &ty, NULL, &cnt, 1) || cnt == 0)
        return false;
    *x = (int16_t)tx;
    *y = (int16_t)ty;
    s_last_touch_us = esp_timer_get_time();
    return true;
}

/* flash .rodata → PSRAM: the PPA can't DMA from memory-mapped flash */
static surf_image mk_image(const void *rodata, int16_t w, int16_t h, int bpp,
                           uint8_t fmt, const surf_hal *hal)
{
    size_t bytes = (size_t)w * h * (size_t)bpp;
    void *px = hal->alloc_image(bytes);
    memcpy(px, rodata, bytes);
    surf_hal_p4_sync(px, bytes);
    return (surf_image){.pixels = px, .w = w, .h = h, .stride = w * bpp,
                        .format = fmt, .opaque = false};
}

static surf_font mk_font(const surf_font *baked, const surf_hal *hal)
{
    surf_font f = *baked;
    f.atlas = mk_image(baked->atlas.pixels, baked->atlas.w, baked->atlas.h, 1,
                       SURF_FMT_A8, hal);
    return f;
}

static void bar_show(int32_t v, void *user)
{
    surf_rect_set_size(user, (int16_t)(1 + (((int64_t)v * 99) >> 16)), 8);
}

void app_main(void)
{
    printf("surfer M2 — ESP32-P4, %" PRIu32 " KB PSRAM free\n",
           (uint32_t)(heap_caps_get_free_size(MALLOC_CAP_SPIRAM) / 1024));

    bench();

    esp_lcd_panel_handle_t panel = NULL;
    esp_lcd_panel_io_handle_t io = NULL;
    bsp_display_config_t disp_cfg = {
        .dsi_bus = {
            .phy_clk_src = MIPI_DSI_PHY_CLK_SRC_DEFAULT,
            .lane_bit_rate_mbps = BSP_LCD_MIPI_DSI_LANE_BITRATE_MBPS,
        },
    };
    if (bsp_display_new(&disp_cfg, &panel, &io) != ESP_OK) {
        printf("display init failed — headless, bench done\n");
        return;
    }
    bsp_display_backlight_on();
    void *scan_fb0 = NULL, *scan_fb1 = NULL, *scan_fb2 = NULL;
    ESP_ERROR_CHECK(
        esp_lcd_dpi_panel_get_frame_buffer(panel, 3, &scan_fb0, &scan_fb1, &scan_fb2));

    /* bring-up sanity: CPU color bars via the official draw path */
    {
        static const uint16_t bars[8] = {0xf800, 0x07e0, 0x001f, 0xffe0,
                                         0x07ff, 0xf81f, 0xffff, 0x8410};
        uint16_t *pat = heap_caps_malloc(LCD_W * LCD_H * 2, MALLOC_CAP_SPIRAM);
        for (int y = 0; y < LCD_H; y++)
            for (int x = 0; x < LCD_W; x++)
                pat[y * LCD_W + x] = bars[x * 8 / LCD_W];
        ESP_ERROR_CHECK(esp_lcd_panel_draw_bitmap(panel, 0, 0, LCD_W, LCD_H, pat));
        printf("TESTPATTERN drawn — 8 vertical color bars should be visible\n");
        vTaskDelay(pdMS_TO_TICKS(1500));
        heap_caps_free(pat);
    }
    if (bsp_touch_new(NULL, &s_touch) != ESP_OK) {
        printf("touch init failed — autodrag only\n");
        s_touch = NULL;
    }

    surf_hal_p4_cfg cfg = {
        .panel = panel, .scan_fbs = {scan_fb0, scan_fb1, scan_fb2},
        .w = LCD_W, .h = LCD_H,
        .touch_poll = touch_poll,
    };
    const surf_hal *hal = surf_hal_p4_init(&cfg);
    ESP_ERROR_CHECK(hal ? ESP_OK : ESP_FAIL);

    surf_config scfg = {.max_nodes = 128, .bg = SURF_RGB(24, 26, 32)};
    surf_init(hal, LCD_W, LCD_H, &scfg);

    surf_image knob_img = mk_image(widget_knob_px, WKNOB_STRIP_W, WKNOB_SIZE, 4,
                                   SURF_FMT_ARGB8888, hal);
    surf_image track_img = mk_image(widget_trackfull_px, WTRACKFULL_W, WTRACKFULL_H, 4,
                                    SURF_FMT_ARGB8888, hal);
    surf_image cap_img = mk_image(widget_cap_px, WCAP_W, WCAP_H, 4,
                                  SURF_FMT_ARGB8888, hal);
    surf_font ui16 = mk_font(&surf_font_ui16, hal);
    surf_font ui28 = mk_font(&surf_font_ui28, hal);
    surf_knob_style kstyle = {.strip = &knob_img, .frame_w = WKNOB_SIZE,
                              .frame_h = WKNOB_SIZE, .frames = WKNOB_FRAMES};
    surf_slider_style sstyle = {.track = &track_img, .inset = WTRACK_INSET, .cap = &cap_img};

    surf_node_add(surf_screen(), surf_rect_new(0, 0, LCD_W, 40, SURF_RGB(38, 42, 52)));
    surf_node_add(surf_screen(), surf_rect_new(0, LCD_H - 56, LCD_W, 56, SURF_RGB(38, 42, 52)));
    surf_node_add(surf_screen(),
                  surf_text_new(&ui28, "surfer mixer", 12, 2, SURF_RGB(240, 242, 248)));

    surf_knob *knobs[N];
    surf_slider *sliders[N];
    surf_node *kbar[N], *sbar[N];
    for (int i = 0; i < N; i++) {
        int16_t kx = (int16_t)(90 + i * 160);
        knobs[i] = surf_knob_new(surf_screen(), kx, 70, &kstyle);
        sliders[i] = surf_slider_new(surf_screen(), (int16_t)(kx + 8), 180,
                                     WTRACKFULL_W, WTRACKFULL_H, &sstyle);
        kbar[i] = surf_rect_new(kx, 24, 1, 8, SURF_RGB(240, 190, 80));
        sbar[i] = surf_rect_new(kx, (int16_t)(LCD_H - 28), 1, 8, SURF_RGB(80, 200, 220));
        surf_node_add(surf_screen(), kbar[i]);
        surf_node_add(surf_screen(), sbar[i]);

        surf_node *name = surf_text_new(&ui16, knob_names[i], kx, 140,
                                        SURF_RGB(180, 186, 198));
        surf_text_set_wrap(name, WKNOB_SIZE);
        surf_text_set_align(name, SURF_ALIGN_CENTER);
        surf_node_add(surf_screen(), name);
        surf_knob_on_change(knobs[i], bar_show, kbar[i]);
        surf_slider_on_change(sliders[i], bar_show, sbar[i]);
        surf_knob_set_value(knobs[i], i * SURF_ONE / (N - 1));
        surf_slider_set_value(sliders[i], SURF_ONE - i * SURF_ONE / (N - 1));
        bar_show(surf_knob_value(knobs[i]), kbar[i]);
        bar_show(surf_slider_value(sliders[i]), sbar[i]);
    }

    printf("mixer up: %dx%d, touch %s — stats 1/s\n", LCD_W, LCD_H,
           s_touch ? "on" : "off");

    int32_t phase = 0;
    int64_t acc = 0, worst = 0, win_start = esp_timer_get_time();
    int win_frames = 0;

    for (;;) {
        bool autodrag = (esp_timer_get_time() - s_last_touch_us) > 2000000;
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

        int64_t t0 = esp_timer_get_time();
        surf_tick();
        int64_t dt = esp_timer_get_time() - t0;
        acc += dt;
        if (dt > worst)
            worst = dt;
        win_frames++;

        int64_t now = esp_timer_get_time();
        if (now - win_start >= 1000000) {
            printf("tick avg %.2f ms  max %.2f ms  %.1f fps  [%s]\n",
                   acc / 1000.0 / win_frames, worst / 1000.0,
                   win_frames * 1e6 / (double)(now - win_start),
                   autodrag ? "autodrag" : "touch");
            acc = worst = 0;
            win_frames = 0;
            win_start = now;
        }

        /* triple-buffered present never blocks; pace the loop to ~60 Hz */
        int64_t budget_ms = (16667 - dt) / 1000;
        vTaskDelay(pdMS_TO_TICKS(budget_ms < 1 ? 1 : budget_ms));
    }
}
