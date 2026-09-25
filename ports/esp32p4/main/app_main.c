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
#include "driver/gpio.h"
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
#include "fonts_scene.h"

/* Which scene this image runs. */
#define DEMO_MIXER  0   /* M2: 6 knobs + 6 sliders */
#define DEMO_EDITOR 1   /* full-screen textgrid scroll test (CPU fast path) */
#define DEMO_FONTS  2   /* font specimen — same scene as build/surfer_fonts */
#define DEMO_MODE   DEMO_FONTS

/* editor A/B: 1 = single-buffer direct (no flip/copy — fastest scroll,
 * possible shear as the shift races scanout); 0 = triple-buffer (51fps,
 * artifact-free) */
#define EDITOR_SINGLE_BUFFER 1


static const char *knob_names[6] = {"cutoff", "res", "env", "lfo", "mix", "vol"};

#define BENCH_W 1920
#define BENCH_H 1080
#define LCD_W   1024
#define LCD_H   600
#define N       6

/* Panel reset and backlight are NOT on the BSP's stock pins on our bench
 * board. Stock is GPIO 27 (reset) and 26 (backlight, LEDC PWM), but 26/27
 * are the Full-Speed OTG PHY D-/D+ — the port tulip2 runs USB host on, so
 * a FS keyboard can enumerate behind a hub without a Transaction
 * Translator. Both signals are therefore jumpered to 4/5 (dupont from the
 * header to the display header, no solder mod); same wiring on both EV
 * boards. Values lifted from tulip2 drivers/port_p4.c, verified there.
 *
 * The BSP hardcodes its pins in the header with no Kconfig override, so
 * we reset and light the panel ourselves and skip
 * bsp_display_backlight_on(). bsp_display_new() still toggles GPIO 27 on
 * its own; harmless here since this firmware runs no USB host.
 *
 * Set to 0 for a stock, un-jumpered EV board. */
#define BENCH_JUMPERED_PANEL 1
#if BENCH_JUMPERED_PANEL
#define PIN_BACKLIGHT 4   /* jumpered off 26: FS-USB D- */
#define PIN_LCD_RST   5   /* jumpered off 27: FS-USB D+ */

static void jumpered_panel_reset(void)
{
    /* Active low, tulip2's timings. Safe to do before bsp_display_new:
     * the panel module has its own supply (the P4's LDO_VO3 feeds
     * VDD_MIPI_DPHY, the SoC PHY — not the panel), so it is already
     * powered and ready to accept a reset pulse at this point. */
    gpio_config_t rst = {
        .mode = GPIO_MODE_OUTPUT,
        .pin_bit_mask = 1ULL << PIN_LCD_RST,
    };
    ESP_ERROR_CHECK(gpio_config(&rst));
    gpio_set_level(PIN_LCD_RST, 1);
    vTaskDelay(pdMS_TO_TICKS(10));
    gpio_set_level(PIN_LCD_RST, 0);
    vTaskDelay(pdMS_TO_TICKS(10));
    gpio_set_level(PIN_LCD_RST, 1);
    vTaskDelay(pdMS_TO_TICKS(120));
}

static void jumpered_backlight_on(void)
{
    gpio_config_t bl = {
        .mode = GPIO_MODE_OUTPUT,
        .pin_bit_mask = 1ULL << PIN_BACKLIGHT,
    };
    ESP_ERROR_CHECK(gpio_config(&bl));
    gpio_set_level(PIN_BACKLIGHT, 1);
}
#endif

/* ---- phase A: bandwidth + op overhead ---- */
#if DEMO_MODE != DEMO_FONTS

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
#endif /* DEMO_MODE != DEMO_FONTS */

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

/* surf_font_builtin_prepare hook: the PPA can't DMA from memory-mapped
 * flash, so every built-in atlas is copied into PSRAM once at startup and
 * the registry keeps the prepared copy. */
static const surf_hal *s_hal;

static void psram_prepare_atlas(surf_image *img)
{
    size_t bytes = (size_t)img->stride * img->h;
    void *px = s_hal->alloc_image(bytes);
    if (!px)
        return;
    memcpy(px, img->pixels, bytes);
    surf_hal_p4_sync(px, bytes);
    img->pixels = px;
}

static void bar_show(int32_t v, void *user)
{
    surf_rect_set_size(user, (int16_t)(1 + (((int64_t)v * 99) >> 16)), 8);
}

#if DEMO_MODE == DEMO_EDITOR
/* ---- editor scroll test: the textgrid worst case, one line per frame,
 * every cell rewritten — the on-device answer to DESIGN.md §5.6's
 * predicted 15-20 ms/page. Finger-drag scrolls; idle resumes auto. */

static const char *code_lines[] = {
    "static bool collect(surf_node *n, int16_t px, int16_t py,",
    "                    surf_rect clip, surf_rect dr)",
    "{",
    "    if (n->flags & SURF_NF_HIDDEN)",
    "        return false;",
    "    int16_t ax = (int16_t)(px + n->x), ay = (int16_t)(py + n->y);",
    "",
    "    if (n->type == SURF_NODE_GROUP) {",
    "        if (n->flags & SURF_NF_CLIP) {",
    "            clip = surf_rect_intersect(clip, box);",
    "            if (surf_rect_empty(clip))",
    "                return false;",
    "        }",
    "        for (surf_node *c = n->last; c; c = c->prev)",
    "            if (collect(c, ax, ay, clip, dr))",
    "                return true;",
    "        return false;",
    "    }",
    "",
    "    surf_rect bounds = {ax, ay, n->w, n->h};",
    "    surf_rect vis = surf_rect_intersect(bounds, clip);",
    "    if (surf_rect_empty(vis))",
    "        return false;",
    "",
    "    surf_g.plist[paint_n++] = (surf_paint_ent){n, ax, ay, vis};",
    "    return node_opaque(n) && surf_rect_covers(bounds, dr);",
    "}",
};
#define NCODE (int)(sizeof code_lines / sizeof code_lines[0])

static surf_node *ed_grid;
static int16_t ed_rows, ed_cols;
static int ed_top;
static int64_t ed_last_touch;
static int ed_drag_acc, ed_last_y;
static int16_t ed_cell_h;

static void ed_fill_row(int16_t row, int lineno)
{
    char buf[160];
    snprintf(buf, sizeof buf, "%5d  %s", lineno + 1,
             code_lines[lineno % NCODE]);
    surf_textgrid_set_row(ed_grid, row, buf);
}

static void ed_fill_all(void)
{
    for (int16_t r = 0; r < ed_rows; r++)
        ed_fill_row(r, ed_top + r);
}

static void ed_touch(surf_node *n, const surf_touch *t, void *user)
{
    (void)n; (void)user;
    ed_last_touch = esp_timer_get_time();
    if (t->phase == SURF_TOUCH_DOWN) {
        ed_last_y = t->y;
        ed_drag_acc = 0;
        return;
    }
    if (t->phase != SURF_TOUCH_MOVE)
        return;
    ed_drag_acc += ed_last_y - t->y;
    ed_last_y = t->y;
    while (ed_drag_acc >= ed_cell_h) {
        ed_drag_acc -= ed_cell_h;
        ed_top++;
        surf_textgrid_scroll(ed_grid, 1);
        ed_fill_row((int16_t)(ed_rows - 1), ed_top + ed_rows - 1);
    }
    while (ed_drag_acc <= -ed_cell_h && ed_top > 0) {
        ed_drag_acc += ed_cell_h;
        ed_top--;
        surf_textgrid_scroll(ed_grid, -1);
        ed_fill_row(0, ed_top);
    }
}

static void editor_scene(const surf_hal *hal, const surf_font *mono)
{
    surf_point cs;
    ed_cell_h = surf_font_line_h(mono);
    {
        surf_node *probe = surf_textgrid_new(mono, 1, 1, 0, 0);
        cs = surf_textgrid_cell_size(probe);
        surf_node_destroy(probe);
    }
    ed_cols = (int16_t)(LCD_W / cs.x);
    ed_rows = (int16_t)(LCD_H / cs.y);
    ed_grid = surf_textgrid_new(mono, ed_cols, ed_rows,
                                SURF_RGB(200, 205, 215), SURF_RGB(18, 20, 25));
    surf_node_add(surf_screen(), ed_grid);
    surf_node_set_on_touch(ed_grid, ed_touch, NULL);
    surf_node_set_gesture_grab(ed_grid, true);
    surf_textgrid_set_fast_scroll(ed_grid, true);  /* fullscreen, unoccluded */
    ed_fill_all();

    printf("editor up: %dx%d cells (%dx%d px each) — drag to scroll, "
           "idle 2s resumes autoscroll\n", ed_cols, ed_rows, cs.x, cs.y);

    int64_t acc = 0, worst = 0, win_start = esp_timer_get_time();
    int win_frames = 0;

    for (;;) {
        bool autoscroll =
            (esp_timer_get_time() - ed_last_touch) > 2000000;
        if (autoscroll) {
            ed_top++;
            surf_textgrid_scroll(ed_grid, 1);
            ed_fill_row((int16_t)(ed_rows - 1), ed_top + ed_rows - 1);
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
                   autoscroll ? "autoscroll" : "touch");
            acc = worst = 0;
            win_frames = 0;
            win_start = now;
        }
        vTaskDelay(1);
    }
}
#endif /* DEMO_MODE == DEMO_EDITOR */

void app_main(void)
{
    printf("surfer M2 — ESP32-P4, %" PRIu32 " KB PSRAM free\n",
           (uint32_t)(heap_caps_get_free_size(MALLOC_CAP_SPIRAM) / 1024));

#if DEMO_MODE != DEMO_FONTS
    bench();  /* ~20 s of allocation and blitting; the specimen skips it */
#endif

    esp_lcd_panel_handle_t panel = NULL;
    esp_lcd_panel_io_handle_t io = NULL;
    /* phy_clk_src is deliberately left 0. On IDF >= 5.5.3
     * MIPI_DSI_PHY_CLK_SRC_DEFAULT is a compat #define for the LEGACY
     * PLL_F20M reference, which is not a legal PLL ref on rev v3.x
     * ("P4X") silicon — it compiles clean and then hits default: abort()
     * inside esp_lcd_new_dsi_bus at boot, with no message. Left at 0 the
     * driver picks PLL_F20M for v1.x and XTAL for v3.x from the
     * configured minimum revision, so one line is right for both. */
    bsp_display_config_t disp_cfg = {
        .dsi_bus = {
            .lane_bit_rate_mbps = BSP_LCD_MIPI_DSI_LANE_BITRATE_MBPS,
        },
    };
#if BENCH_JUMPERED_PANEL
    jumpered_panel_reset();   /* before init cmds — the BSP's pin is dead */
#endif
    if (bsp_display_new(&disp_cfg, &panel, &io) != ESP_OK) {
        printf("display init failed — headless, bench done\n");
        return;
    }
#if BENCH_JUMPERED_PANEL
    jumpered_backlight_on();
    printf("panel: reset on GPIO %d, backlight on GPIO %d (jumpered)\n",
           PIN_LCD_RST, PIN_BACKLIGHT);
#else
    bsp_display_backlight_on();
#endif
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
#if DEMO_MODE == DEMO_EDITOR && EDITOR_SINGLE_BUFFER
        .single_buffer = true,
#endif
    };
    const surf_hal *hal = surf_hal_p4_init(&cfg);
    ESP_ERROR_CHECK(hal ? ESP_OK : ESP_FAIL);

    surf_config scfg = {.max_nodes = 256, .bg = SURF_RGB(24, 26, 32)};
    surf_init(hal, LCD_W, LCD_H, &scfg);

    surf_image knob_img = mk_image(widget_knob_px, WKNOB_STRIP_W, WKNOB_SIZE, 4,
                                   SURF_FMT_ARGB8888, hal);
    surf_image track_img = mk_image(widget_trackfull_px, WTRACKFULL_W, WTRACKFULL_H, 4,
                                    SURF_FMT_ARGB8888, hal);
    surf_image cap_img = mk_image(widget_cap_px, WCAP_W, WCAP_H, 4,
                                  SURF_FMT_ARGB8888, hal);
    s_hal = hal;
    surf_font_builtin_prepare(psram_prepare_atlas);
    const surf_font *ui16 = surf_font_builtin("ui16");
    const surf_font *ui28 = surf_font_builtin("ui28");

#if DEMO_MODE == DEMO_EDITOR
    (void)ui16; (void)ui28;
    (void)knob_img; (void)track_img; (void)cap_img;
    editor_scene(hal, surf_font_builtin("mono16"));  /* never returns */
#elif DEMO_MODE == DEMO_FONTS
    (void)ui16; (void)ui28;
    (void)knob_img; (void)track_img; (void)cap_img;
    fonts_scene_build(LCD_W, LCD_H, "ESP32-P4 - EK79007 1024x600 panel", 3);
    printf("font specimen up: %dx%d, touch %s\n", LCD_W, LCD_H,
           s_touch ? "on" : "off");

    /* static page: the first tick paints it, later ticks find no damage.
     * Keep ticking anyway so touch stays live and the triple buffer
     * finishes forwarding damage into all three scan buffers. */
    for (;;) {
        surf_tick();
        vTaskDelay(pdMS_TO_TICKS(16));
    }
#endif
    surf_knob_style kstyle = {.strip = &knob_img, .frame_w = WKNOB_SIZE,
                              .frame_h = WKNOB_SIZE, .frames = WKNOB_FRAMES};
    surf_slider_style sstyle = {.track = &track_img, .inset = WTRACK_INSET, .cap = &cap_img};

    surf_node_add(surf_screen(), surf_rect_new(0, 0, LCD_W, 40, SURF_RGB(38, 42, 52)));
    surf_node_add(surf_screen(), surf_rect_new(0, LCD_H - 56, LCD_W, 56, SURF_RGB(38, 42, 52)));
    surf_node_add(surf_screen(),
                  surf_text_new(ui28, "surfer mixer", 12, 2, SURF_RGB(240, 242, 248)));

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

        surf_node *name = surf_text_new(ui16, knob_names[i], kx, 140,
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
