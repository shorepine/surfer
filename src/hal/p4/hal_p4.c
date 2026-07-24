/* ESP32-P4 hal: every frame op is a PPA job. fill → fill engine, blit →
 * SRM (1:1, with ARGB8888→RGB565 conversion for free), blend → blend
 * engine (per-pixel fg alpha over RGB565 bg). Blocking transfer mode
 * keeps cross-engine ordering correct; M2 measures whether queueing is
 * ever needed. CPU never touches the compose buffer, so the only cache
 * sync in the system is surf_hal_p4_sync() after asset uploads. */
#include <string.h>

#include "driver/ppa.h"
#include "esp_async_fbcpy.h"  /* esp_lcd priv_include: DMA2D rect copy */
#include "esp_cache.h"
#include "esp_heap_caps.h"
#include "esp_lcd_mipi_dsi.h"
#include "esp_lcd_panel_ops.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "hal/color_types.h"

#include "hal_p4.h"

/* P4 L2 cache line; alignment covers the hal's 64-byte contract too. */
#define P4_ALIGN 128

/* ≥ core's dirty-list cap (present never receives more rects than that;
 * a shorter list here truncates prev_r and forces full-copy forwarding) */
#define SURF_MAX_DIRTY_P4 32

static struct {
    surf_hal_p4_cfg      cfg;
    uint16_t            *fb;       /* compose target = back buffer, RGB565 */
    size_t               fb_bytes;
    int                  nfbs;     /* 0 headless-alloc, 1 direct, 3 flip */
    int                  back;
    volatile uint8_t     live;      /* buffer the DPI DMA latched last vsync */
    uint8_t              last_flip; /* most recently flipped (newest frame) */
    surf_rect            prev_r[SURF_MAX_DIRTY_P4];
    int                  prev_n;
    bool                 prev_overflow;  /* prev list truncated: unusable */
    uint32_t             frame_no;
    uint32_t             fb_stamp[3];    /* frame each buffer is current to */
    ppa_client_handle_t  fill_cl, srm_cl, blend_cl;
    esp_async_fbcpy_handle_t fbcpy;
    SemaphoreHandle_t    fbcpy_sem;
    bool                 scrolled;  /* force a full damage-forward at present */
    volatile uint32_t    vsync_count;
    SemaphoreHandle_t    vsync_sem;
    uint32_t             pace_ref;   /* vsync count anchoring the frame lock */
    int64_t              hz_t0_us;   /* refresh-rate measurement anchor */
    uint32_t             hz_c0;
    int32_t              shift_acc; /* net scroll_rect shift since last present */
    /* touch edge state */
    bool                 was_down;
    int16_t              last_x, last_y;
    uint8_t              empty_reads;  /* release hysteresis counter */
} S;

static void h_fill(surf_rect dst, surf_color c)
{
    ppa_fill_oper_config_t op = {
        .out = {
            .buffer = S.fb,
            .buffer_size = S.fb_bytes,
            .pic_w = (uint32_t)S.cfg.w,
            .pic_h = (uint32_t)S.cfg.h,
            .block_offset_x = (uint32_t)dst.x,
            .block_offset_y = (uint32_t)dst.y,
            .fill_cm = PPA_FILL_COLOR_MODE_RGB565,
        },
        .fill_block_w = (uint32_t)dst.w,
        .fill_block_h = (uint32_t)dst.h,
        .fill_argb_color = {
            .a = 0xff,
            .r = (uint8_t)(((c >> 11) << 3) | ((c >> 13) & 0x7)),
            .g = (uint8_t)(((c >> 5) << 2) & 0xfc),
            .b = (uint8_t)((c << 3) & 0xf8),
        },
        .mode = PPA_TRANS_MODE_BLOCKING,
    };
    ppa_do_fill(S.fill_cl, &op);
}

static ppa_srm_color_mode_t srm_cm(const surf_image *img)
{
    return img->format == SURF_FMT_ARGB8888 ? PPA_SRM_COLOR_MODE_ARGB8888
                                            : PPA_SRM_COLOR_MODE_RGB565;
}

static void srm_copy(const surf_image *src, surf_rect sr, void *dst_buf,
                     size_t dst_bytes, int dst_pic_w, int dst_pic_h, surf_point dst)
{
    ppa_srm_oper_config_t op = {
        .in = {
            .buffer = src->pixels,
            .pic_w = (uint32_t)(src->stride / (src->format == SURF_FMT_ARGB8888 ? 4 : 2)),
            .pic_h = (uint32_t)src->h,
            .block_w = (uint32_t)sr.w,
            .block_h = (uint32_t)sr.h,
            .block_offset_x = (uint32_t)sr.x,
            .block_offset_y = (uint32_t)sr.y,
            .srm_cm = srm_cm(src),
        },
        .out = {
            .buffer = dst_buf,
            .buffer_size = dst_bytes,
            .pic_w = (uint32_t)dst_pic_w,
            .pic_h = (uint32_t)dst_pic_h,
            .block_offset_x = (uint32_t)dst.x,
            .block_offset_y = (uint32_t)dst.y,
            .srm_cm = PPA_SRM_COLOR_MODE_RGB565,
        },
        .rotation_angle = PPA_SRM_ROTATION_ANGLE_0,
        .scale_x = 1.0f,
        .scale_y = 1.0f,
        .mode = PPA_TRANS_MODE_BLOCKING,
    };
    ppa_do_scale_rotate_mirror(S.srm_cl, &op);
}

static void h_blit(const surf_image *src, surf_rect sr, surf_point dst)
{
    srm_copy(src, sr, S.fb, S.fb_bytes, S.cfg.w, S.cfg.h, dst);
}

static int bytespp(const surf_image *img)
{
    switch (img->format) {
    case SURF_FMT_ARGB8888: return 4;
    case SURF_FMT_A8:       return 1;
    default:                return 2;
    }
}

static ppa_blend_color_mode_t blend_cm(const surf_image *img)
{
    switch (img->format) {
    case SURF_FMT_ARGB8888: return PPA_BLEND_COLOR_MODE_ARGB8888;
    case SURF_FMT_A8:       return PPA_BLEND_COLOR_MODE_A8;
    default:                return PPA_BLEND_COLOR_MODE_RGB565;
    }
}

static void h_blend(const surf_image *src, surf_rect sr, surf_point dst, uint8_t opa)
{
    ppa_blend_oper_config_t op = {
        .in_bg = {
            .buffer = S.fb,
            .pic_w = (uint32_t)S.cfg.w,
            .pic_h = (uint32_t)S.cfg.h,
            .block_w = (uint32_t)sr.w,
            .block_h = (uint32_t)sr.h,
            .block_offset_x = (uint32_t)dst.x,
            .block_offset_y = (uint32_t)dst.y,
            .blend_cm = PPA_BLEND_COLOR_MODE_RGB565,
        },
        .in_fg = {
            .buffer = src->pixels,
            .pic_w = (uint32_t)(src->stride / bytespp(src)),
            .pic_h = (uint32_t)src->h,
            .block_w = (uint32_t)sr.w,
            .block_h = (uint32_t)sr.h,
            .block_offset_x = (uint32_t)sr.x,
            .block_offset_y = (uint32_t)sr.y,
            .blend_cm = blend_cm(src),
        },
        .out = {
            .buffer = S.fb,
            .buffer_size = S.fb_bytes,
            .pic_w = (uint32_t)S.cfg.w,
            .pic_h = (uint32_t)S.cfg.h,
            .block_offset_x = (uint32_t)dst.x,
            .block_offset_y = (uint32_t)dst.y,
            .blend_cm = PPA_BLEND_COLOR_MODE_RGB565,
        },
        .bg_alpha_update_mode = PPA_ALPHA_FIX_VALUE,
        .bg_alpha_fix_val = 0xff,
        .fg_alpha_update_mode = opa == 255 ? PPA_ALPHA_NO_CHANGE : PPA_ALPHA_SCALE,
        .fg_alpha_scale_ratio = (float)opa / 255.0f,
        /* A8 (glyph atlases): alpha from the data, color from the tint */
        .fg_fix_rgb_val = {
            .r = (uint8_t)(((src->tint >> 11) << 3) | ((src->tint >> 13) & 0x7)),
            .g = (uint8_t)(((src->tint >> 5) << 2) & 0xfc),
            .b = (uint8_t)((src->tint << 3) & 0xf8),
        },
        .mode = PPA_TRANS_MODE_BLOCKING,
    };
    ppa_do_blend(S.blend_cl, &op);
}

/* Transformed sprites: the PPA's SRM engine scales/rotates but cannot
 * blend, and the blend engine cannot scale — so SRM the whole sprite
 * into a scratch buffer, then blend the visible sub-rect over the fb.
 * Two PPA ops ≈ 200–400µs per sprite per damaged frame. Scratch grows
 * lazily and is PPA-to-PPA only (no CPU access → no cache sync). */
static struct {
    void  *buf;
    size_t bytes;
} S_xform;

static void *h_alloc_image(size_t bytes);
static void h_free_image(void *p);

static void h_xform_blend(const surf_image *src, surf_rect sr, surf_rect dst_r,
                          surf_rect vis, uint8_t rot, uint8_t mirror)
{
    int bpp = bytespp(src);
    int32_t stride = ((int32_t)dst_r.w * bpp + P4_ALIGN - 1) & ~(P4_ALIGN - 1);
    size_t need = (size_t)stride * dst_r.h;
    if (need > S_xform.bytes) {
        if (S_xform.buf)
            h_free_image(S_xform.buf);
        S_xform.buf = h_alloc_image(need);
        S_xform.bytes = S_xform.buf ? need : 0;
        if (!S_xform.buf)
            return;
    }

    /* Clear the scratch block first: the SRM's output size is
     * floor(src * scale) in its own fixed point, and when that lands a
     * pixel short of our footprint the right column / bottom row of the
     * scratch would otherwise keep the PREVIOUS frame's pixels — a
     * ship-colored fringe on rotated sprites. Transparent black makes
     * the short edge simply not blend (ARGB); 565 gets black. */
    ppa_fill_oper_config_t clr = {
        .out = {
            .buffer = S_xform.buf,
            .buffer_size = S_xform.bytes,
            .pic_w = (uint32_t)(stride / bpp),
            .pic_h = (uint32_t)dst_r.h,
            .block_offset_x = 0,
            .block_offset_y = 0,
            .fill_cm = src->format == SURF_FMT_ARGB8888
                           ? PPA_FILL_COLOR_MODE_ARGB8888
                           : PPA_FILL_COLOR_MODE_RGB565,
        },
        .fill_block_w = (uint32_t)dst_r.w,
        .fill_block_h = (uint32_t)dst_r.h,
        .fill_argb_color = {.val = 0},
        .mode = PPA_TRANS_MODE_BLOCKING,
    };
    ppa_do_fill(S.fill_cl, &clr);

    /* footprint before rotation — SRM wants pre-rotation scale factors */
    int32_t w0 = (rot & 1) ? dst_r.h : dst_r.w;
    int32_t h0 = (rot & 1) ? dst_r.w : dst_r.h;
    ppa_srm_oper_config_t srm = {
        .in = {
            .buffer = src->pixels,
            .pic_w = (uint32_t)(src->stride / bpp),
            .pic_h = (uint32_t)src->h,
            .block_w = (uint32_t)sr.w,
            .block_h = (uint32_t)sr.h,
            .block_offset_x = (uint32_t)sr.x,
            .block_offset_y = (uint32_t)sr.y,
            .srm_cm = srm_cm(src),
        },
        .out = {
            .buffer = S_xform.buf,
            .buffer_size = S_xform.bytes,
            .pic_w = (uint32_t)(stride / bpp),
            .pic_h = (uint32_t)dst_r.h,
            .block_offset_x = 0,
            .block_offset_y = 0,
            .srm_cm = srm_cm(src),
        },
        .rotation_angle = (ppa_srm_rotation_angle_t)rot,
        .scale_x = (float)w0 / (float)sr.w,
        .scale_y = (float)h0 / (float)sr.h,
        .mirror_x = (mirror & 1) != 0,
        .mirror_y = (mirror & 2) != 0,
        .mode = PPA_TRANS_MODE_BLOCKING,
    };
    ppa_do_scale_rotate_mirror(S.srm_cl, &srm);

    surf_image scratch = {
        .pixels = S_xform.buf,
        .w = dst_r.w,
        .h = dst_r.h,
        .stride = stride,
        .format = src->format,
        .opaque = src->opaque,
        .tint = src->tint,
    };
    if (src->opaque)
        h_blit(&scratch,
               (surf_rect){(int16_t)(vis.x - dst_r.x), (int16_t)(vis.y - dst_r.y),
                           vis.w, vis.h},
               (surf_point){vis.x, vis.y});
    else
        h_blend(&scratch,
                (surf_rect){(int16_t)(vis.x - dst_r.x), (int16_t)(vis.y - dst_r.y),
                            vis.w, vis.h},
                (surf_point){vis.x, vis.y}, 255);
}

static bool fbcpy_done(esp_async_fbcpy_handle_t mcp, esp_async_fbcpy_event_data_t *ev,
                       void *arg)
{
    (void)mcp; (void)ev;
    BaseType_t hp = pdFALSE;
    xSemaphoreGiveFromISR((SemaphoreHandle_t)arg, &hp);
    return hp == pdTRUE;
}

/* The end-of-frame ISR is when the DPI DMA latches the most recently
 * flipped buffer; recording it tells present which buffer is on glass. */
static bool vsync_cb(esp_lcd_panel_handle_t panel, esp_lcd_dpi_panel_event_data_t *ev,
                     void *arg)
{
    (void)panel; (void)ev; (void)arg;
    S.live = S.last_flip;
    S.vsync_count++;
    BaseType_t hp = pdFALSE;
    if (S.vsync_sem)
        xSemaphoreGiveFromISR(S.vsync_sem, &hp);
    return hp == pdTRUE;
}

/* Frame lock: block until `divisor` panel refreshes have passed since
 * the anchor (60/divisor fps). Early frames wait on the vsync ISR; a
 * late frame slips whole periods and re-anchors — no burst catch-up,
 * so cadence stays quantized to the panel. */
/* Measured, not configured: the DSI PLL rounds the requested DPI clock
 * (52 MHz asked, 60 granted on this board -> 69.7 Hz, not the 60 the
 * timing math promises). Count real vsyncs against the clock; gather
 * at least ~15 refreshes before answering. */
static float h_frame_hz(void)
{
    if (S.nfbs != 3)
        return 60.0f;
    int64_t dt = esp_timer_get_time() - S.hz_t0_us;
    uint32_t dc = S.vsync_count - S.hz_c0;
    if (dc < 15) {
        vTaskDelay(pdMS_TO_TICKS(250));
        dt = esp_timer_get_time() - S.hz_t0_us;
        dc = S.vsync_count - S.hz_c0;
    }
    if (dt <= 0 || dc == 0)
        return 60.0f;
    return (float)((double)dc * 1e6 / (double)dt);
}

static void h_wait_frame(int divisor)
{
    if (S.nfbs != 3 || !S.vsync_sem || divisor <= 0)
        return;
    uint32_t target = S.pace_ref + (uint32_t)divisor;
    if ((int32_t)(S.vsync_count - target) >= 0) {
        S.pace_ref = S.vsync_count;
        return;
    }
    /* BOUNDED wait: if vsyncs stop arriving (DSI stall), give up after
     * ~240ms and resync rather than wedging the calling task forever —
     * that wedge froze the whole UI task (screen static, REPL dead,
     * audio tasks fine, both cores idle at WFI: the hang-autopsy
     * signature). A stall now degrades to a stutter instead. */
    int dry = 0;
    while ((int32_t)(S.vsync_count - target) < 0) {
        uint32_t before = S.vsync_count;
        xSemaphoreTake(S.vsync_sem, pdMS_TO_TICKS(40));
        if (S.vsync_count == before) {
            if (++dry >= 6) {
                S.pace_ref = S.vsync_count;   /* resync the lock */
                return;
            }
        } else {
            dry = 0;
        }
    }
    S.pace_ref = target;
}

static void fwd_copy(int src_fb, int dst_fb, surf_rect r)
{
    esp_async_fbcpy_trans_desc_t t = {
        .src_buffer = S.cfg.scan_fbs[src_fb],
        .dst_buffer = S.cfg.scan_fbs[dst_fb],
        .src_buffer_size_x = (size_t)S.cfg.w,
        .src_buffer_size_y = (size_t)S.cfg.h,
        .dst_buffer_size_x = (size_t)S.cfg.w,
        .dst_buffer_size_y = (size_t)S.cfg.h,
        .src_offset_x = (size_t)r.x,
        .src_offset_y = (size_t)r.y,
        .dst_offset_x = (size_t)r.x,
        .dst_offset_y = (size_t)r.y,
        .copy_size_x = (size_t)r.w,
        .copy_size_y = (size_t)r.h,
        .pixel_format_unique_id = {
            .color_type_id = COLOR_TYPE_ID(COLOR_SPACE_RGB, COLOR_PIXEL_RGB565),
        },
    };
    if (esp_async_fbcpy(S.fbcpy, &t, fbcpy_done, S.fbcpy_sem) == ESP_OK)
        xSemaphoreTake(S.fbcpy_sem, portMAX_DELAY);
}

/* Flip to the buffer we just composed (draw_bitmap with an in-fb pointer is
 * a zero-copy scanout switch that latches at the next frame boundary),
 * then rotate to a buffer that is neither on glass nor pending — with
 * three buffers one always exists, so present never blocks. The new back
 * holds a frame that is two flips old: DMA2D the previous and current
 * damage forward from the newest frame before returning. */
static void h_present(const surf_rect *dirty, int n)
{
    if (n == 0)
        return;

    int miny = S.cfg.h, maxy = 0;
    for (int i = 0; i < n; i++) {
        if (dirty[i].y < miny) miny = dirty[i].y;
        if (dirty[i].y + dirty[i].h > maxy) maxy = dirty[i].y + dirty[i].h;
    }

    if (S.nfbs < 3) {
        /* single-buffer direct: no flip, but CPU-written rows (textgrid)
         * must still reach physical memory for the scanout */
        if (S.nfbs == 1) {
            uint8_t *band = (uint8_t *)S.fb + (int32_t)miny * S.cfg.w * 2;
            esp_cache_msync(band, (size_t)(maxy - miny) * S.cfg.w * 2,
                            ESP_CACHE_MSYNC_FLAG_DIR_C2M |
                                ESP_CACHE_MSYNC_FLAG_UNALIGNED);
        }
        S.scrolled = false;
        return;
    }
    /* Full region, always: draw_bitmap with a partial y-range msyncs the
     * WRONG rows for the flip (it treats the pointer as the region's
     * top-left, we pass the fb base), and streaming bands change rows
     * the dirty list deliberately doesn't cover — the vertical-pan
     * "screen doesn't scroll" bug. Writeback of clean lines is cheap;
     * PPA/DMA2D content isn't cached at all. */
    esp_lcd_panel_draw_bitmap(S.cfg.panel, 0, 0, S.cfg.w, S.cfg.h,
                              S.cfg.scan_fbs[S.back]);
    (void)miny; (void)maxy;
    S.last_flip = (uint8_t)S.back;

    uint8_t live = S.live;  /* snapshot: the ISR may update it under us */
    int newest = S.back;
    for (int i = 0; i < 3; i++) {
        if (i != live && i != newest) {
            S.back = i;
            break;
        }
    }
    S.fb = S.cfg.scan_fbs[S.back];

    /* After DMA2D writes physical memory behind the CPU's back, resident
     * clean cache lines over that band go stale — and a later partial
     * cell write (typing!) would merge its 128-byte line against 2-frame
     * old neighbors. Invalidate the copied band so the CPU refetches. */
    #define INVALIDATE_BAND(y0, y1) do {                                     \
            uintptr_t lo_ = (uintptr_t)S.fb + (uintptr_t)(y0) * S.cfg.w * 2; \
            uintptr_t hi_ = (uintptr_t)S.fb + (uintptr_t)(y1) * S.cfg.w * 2; \
            lo_ &= ~(uintptr_t)(P4_ALIGN - 1);                               \
            hi_ = (hi_ + P4_ALIGN - 1) & ~(uintptr_t)(P4_ALIGN - 1);         \
            esp_cache_msync((void *)lo_, hi_ - lo_,                          \
                            ESP_CACHE_MSYNC_FLAG_DIR_M2C);                   \
        } while (0)

    /* How far behind is the buffer we're about to compose into? Strict
     * rotation gives age 2 (forward prev + current damage), but when the
     * vsync ISR lags, the rotation can skip a buffer for a cycle and the
     * skipped one falls 3+ frames behind — damage older than the prev
     * list is then unrecoverable rect-by-rect (that was the flickering
     * 1-in-3-frames trail under moving sprites). Age decides:
     *   1  → current damage only (buffer was newest last frame)
     *   2  → prev + current (the steady state)
     *   3+ → full copy (rare; also covers prev-list overflow) */
    S.frame_no++;
    uint32_t age = S.frame_no - S.fb_stamp[S.back];

    if (S.scrolled || age > 2 || (age == 2 && S.prev_overflow)) {
        /* scroll_rect moved pixels outside the damage system's view, or
         * the buffer is too stale: bring it fully current in one copy */
        S.scrolled = false;
        S.shift_acc = 0;
        surf_rect full = {0, 0, S.cfg.w, S.cfg.h};
        fwd_copy(newest, S.back, full);
        INVALIDATE_BAND(0, S.cfg.h);
        S.prev_n = 1;
        S.prev_r[0] = full;
        S.prev_overflow = false;
        S.fb_stamp[newest] = S.frame_no;
        S.fb_stamp[S.back] = S.frame_no;
        return;
    }

    /* previous-frame damage already inside this frame's rects is about to
     * be copied anyway — steady-state animation makes that the whole list */
    int cminy = S.cfg.h, cmaxy = 0;
    if (age == 2) {
        for (int i = 0; i < S.prev_n; i++) {
            const surf_rect p = S.prev_r[i];
            bool covered = false;
            for (int j = 0; j < n && !covered; j++)
                covered = dirty[j].x <= p.x && dirty[j].y <= p.y &&
                          dirty[j].x + dirty[j].w >= p.x + p.w &&
                          dirty[j].y + dirty[j].h >= p.y + p.h;
            if (!covered) {
                fwd_copy(newest, S.back, p);
                if (p.y < cminy) cminy = p.y;
                if (p.y + p.h > cmaxy) cmaxy = p.y + p.h;
            }
        }
    }
    for (int i = 0; i < n; i++) {
        fwd_copy(newest, S.back, dirty[i]);
        if (dirty[i].y < cminy) cminy = dirty[i].y;
        if (dirty[i].y + dirty[i].h > cmaxy) cmaxy = dirty[i].y + dirty[i].h;
    }
    if (cmaxy > cminy)
        INVALIDATE_BAND(cminy, cmaxy);
    #undef INVALIDATE_BAND

    S.prev_overflow = n > SURF_MAX_DIRTY_P4;
    S.prev_n = n < SURF_MAX_DIRTY_P4 ? n : SURF_MAX_DIRTY_P4;
    for (int i = 0; i < S.prev_n; i++)
        S.prev_r[i] = dirty[i];
    S.fb_stamp[newest] = S.frame_no;
    S.fb_stamp[S.back] = S.frame_no;
}

static void h_wait_idle(void)
{
    /* Blocking transfer mode: every job has completed on return. */
}

static uint64_t h_now_us(void)
{
    return (uint64_t)esp_timer_get_time();
}

/* Rate-limit I2C traffic: the controller is polled at most once per 8 ms,
 * and the DOWN→UP edge is synthesized from the pressed level. Release
 * uses hysteresis: the GT911 bounces on finger lift (a blink of "no
 * touch" then contact again), which would synthesize a phantom second
 * tap — toggle buttons visibly flip twice. UP is only declared after
 * several consecutive empty polls (~25 ms); shorter gaps bridge into
 * one continuous gesture, which also keeps drags from micro-dropping. */
#define TOUCH_RELEASE_POLLS 3

#define P4_MAX_CONTACTS 5
static surf_touch_pt s_pts[P4_MAX_CONTACTS];
static int s_npts;

static int h_touch_points(surf_touch_pt *out, int max)
{
    int n = s_npts < max ? s_npts : max;
    for (int i = 0; i < n; i++)
        out[i] = s_pts[i];
    return n;
}

static bool h_poll_touch(surf_touch *out)
{
    if (!S.cfg.touch_poll && !S.cfg.touch_poll_multi)
        return false;

    static int64_t last_read;
    int64_t now = esp_timer_get_time();
    if (now - last_read < 8000)
        return false;
    last_read = now;

    int16_t x, y;
    bool down;
    if (S.cfg.touch_poll_multi) {
        s_npts = S.cfg.touch_poll_multi(s_pts, P4_MAX_CONTACTS);
        down = s_npts > 0;
        if (down) {
            x = s_pts[0].x;
            y = s_pts[0].y;
        }
    } else {
        down = S.cfg.touch_poll(&x, &y);
        s_npts = down ? 1 : 0;
        if (down)
            s_pts[0] = (surf_touch_pt){x, y, 0};
    }
    if (down) {
        S.empty_reads = 0;
        if (!S.was_down) {
            S.was_down = true;
            S.last_x = x;
            S.last_y = y;
            *out = (surf_touch){x, y, SURF_TOUCH_DOWN};
            return true;
        }
        if (x != S.last_x || y != S.last_y) {
            S.last_x = x;
            S.last_y = y;
            *out = (surf_touch){x, y, SURF_TOUCH_MOVE};
            return true;
        }
        return false;
    }
    if (S.was_down && ++S.empty_reads >= TOUCH_RELEASE_POLLS) {
        S.was_down = false;
        S.empty_reads = 0;
        *out = (surf_touch){S.last_x, S.last_y, SURF_TOUCH_UP};
        return true;
    }
    return false;
}

/* CPU access to the current back buffer for the textgrid fast path. The
 * present flip's draw_bitmap msyncs the presented band, so CPU-written
 * cells reach physical memory before scanout or DMA2D read them. */
static void *h_fb_ptr(int32_t *stride_bytes)
{
    *stride_bytes = S.cfg.w * 2;
    return S.fb;
}

/* helper for scroll_rect: one DMA2D strip copy inside the back buffer */
static void shift_strip(surf_rect src, int16_t dst_y)
{
    esp_async_fbcpy_trans_desc_t t = {
        .src_buffer = S.fb,
        .dst_buffer = S.fb,
        .src_buffer_size_x = (size_t)S.cfg.w,
        .src_buffer_size_y = (size_t)S.cfg.h,
        .dst_buffer_size_x = (size_t)S.cfg.w,
        .dst_buffer_size_y = (size_t)S.cfg.h,
        .src_offset_x = (size_t)src.x,
        .src_offset_y = (size_t)src.y,
        .dst_offset_x = (size_t)src.x,
        .dst_offset_y = (size_t)dst_y,
        .copy_size_x = (size_t)src.w,
        .copy_size_y = (size_t)src.h,
        .pixel_format_unique_id = {
            .color_type_id = COLOR_TYPE_ID(COLOR_SPACE_RGB, COLOR_PIXEL_RGB565),
        },
    };
    if (esp_async_fbcpy(S.fbcpy, &t, fbcpy_done, S.fbcpy_sem) == ESP_OK)
        xSemaphoreTake(S.fbcpy_sem, portMAX_DELAY);
}

/* one DMA2D copy from an arbitrary source buffer into the back buffer */
static void cross_copy(void *src_buf, surf_rect src, int16_t dst_x, int16_t dst_y)
{
    esp_async_fbcpy_trans_desc_t t = {
        .src_buffer = src_buf,
        .dst_buffer = S.fb,
        .src_buffer_size_x = (size_t)S.cfg.w,
        .src_buffer_size_y = (size_t)S.cfg.h,
        .dst_buffer_size_x = (size_t)S.cfg.w,
        .dst_buffer_size_y = (size_t)S.cfg.h,
        .src_offset_x = (size_t)src.x,
        .src_offset_y = (size_t)src.y,
        .dst_offset_x = (size_t)dst_x,
        .dst_offset_y = (size_t)dst_y,
        .copy_size_x = (size_t)src.w,
        .copy_size_y = (size_t)src.h,
        .pixel_format_unique_id = {
            .color_type_id = COLOR_TYPE_ID(COLOR_SPACE_RGB, COLOR_PIXEL_RGB565),
        },
    };
    if (esp_async_fbcpy(S.fbcpy, &t, fbcpy_done, S.fbcpy_sem) == ESP_OK)
        xSemaphoreTake(S.fbcpy_sem, portMAX_DELAY);
}

/* Shift pixels inside r by dy (content up when dy > 0) with DMA2D.
 *
 * Triple-buffer mode: the just-presented buffer holds an identical copy
 * of the frame the back buffer started from, so the shift is ONE
 * non-overlapping cross-buffer copy in either direction — a downward
 * self-copy would need |dy|-tall raster-safe strips (dozens of DMA ops
 * for a small finger delta; that asymmetry was a visible stutter when
 * scrolling up). Repeated shifts in a tick accumulate: each copy comes
 * from the pristine source at the net offset, and the core translates
 * pending dirty rects to match.
 *
 * Single-buffer mode: self-copy — up in one pass (dst rows precede src
 * rows in raster order), down in |dy|-tall bottom-up strips.
 *
 * Afterwards the CPU cache over the band is invalidated: the DMA changed
 * physical memory, and a later partial cell write must not merge against
 * stale cached lines. */
static void h_scroll_rect(surf_rect r, int16_t dy)
{
    if (!S.fbcpy || dy == 0)
        return;
    int16_t ady = dy < 0 ? (int16_t)-dy : dy;
    if (ady >= r.h)
        return;

    if (S.nfbs == 3) {
        S.shift_acc += dy;
        int32_t acc = S.shift_acc;
        int32_t aacc = acc < 0 ? -acc : acc;
        if (aacc < r.h) {
            surf_rect src = {
                r.x,
                (int16_t)(r.y + (acc > 0 ? acc : 0)),
                r.w,
                (int16_t)(r.h - aacc),
            };
            cross_copy(S.cfg.scan_fbs[S.last_flip], src, r.x,
                       (int16_t)(r.y + (acc < 0 ? -acc : 0)));
        }
        /* acc >= r.h: nothing survives; the accumulated dirty strips
         * cover the whole viewport and compose repaints it */
    } else if (dy > 0) {
        shift_strip((surf_rect){r.x, (int16_t)(r.y + ady), r.w,
                                (int16_t)(r.h - ady)}, r.y);
    } else {
        int32_t remaining = r.h - ady;
        while (remaining > 0) {
            int32_t hh = ady < remaining ? ady : remaining;
            int32_t sy = r.y + remaining - hh;
            shift_strip((surf_rect){r.x, (int16_t)sy, r.w, (int16_t)hh},
                        (int16_t)(sy + ady));
            remaining -= hh;
        }
    }

    /* Invalidate the band (outward-aligned — M2C refuses unaligned
     * ranges) so later partial cell writes can't merge against stale
     * cached lines. No writeback needed first: CPU framebuffer writes
     * happen only in compose, and the following present's draw_bitmap
     * msyncs the damage band — by the time anyone scrolls, the cache
     * holds no dirty framebuffer lines. */
    uintptr_t lo = (uintptr_t)S.fb + (uintptr_t)r.y * S.cfg.w * 2;
    uintptr_t hi = lo + (uintptr_t)r.h * S.cfg.w * 2;
    lo &= ~(uintptr_t)(P4_ALIGN - 1);
    hi = (hi + P4_ALIGN - 1) & ~(uintptr_t)(P4_ALIGN - 1);
    esp_cache_msync((void *)lo, hi - lo, ESP_CACHE_MSYNC_FLAG_DIR_M2C);
    S.scrolled = true;
}

static void *h_alloc_image(size_t bytes)
{
    return heap_caps_aligned_alloc(P4_ALIGN, (bytes + P4_ALIGN - 1) & ~(size_t)(P4_ALIGN - 1),
                                   MALLOC_CAP_SPIRAM);
}

static void h_free_image(void *p)
{
    heap_caps_free(p);
}

void surf_hal_p4_fb_invalidate(void)
{
    if (S.fb)
        esp_cache_msync(S.fb, (S.fb_bytes + P4_ALIGN - 1) & ~(size_t)(P4_ALIGN - 1),
                        ESP_CACHE_MSYNC_FLAG_DIR_M2C);
}

void surf_hal_p4_sync(const void *buf, size_t bytes)
{
    esp_cache_msync((void *)buf, (bytes + P4_ALIGN - 1) & ~(size_t)(P4_ALIGN - 1),
                    ESP_CACHE_MSYNC_FLAG_DIR_C2M);
}

/* Streaming band shift (layers): ONE cross-buffer DMA2D copy from the
 * just-presented frame at the shifted offset — no overlap hazard in
 * either direction, and per the hal contract present does NOT forward
 * the band (next frame's shift rebuilds it from newest). Triple-buffer
 * only: single-buffer has no pristine source, so layers there fall back
 * to full repaint (the vtable entry is nulled at init). */
static void h_band_shift(surf_rect r, int16_t sx, int16_t sy)
{
    if (!S.fbcpy || S.nfbs != 3)
        return;
    int16_t asx = (int16_t)(sx < 0 ? -sx : sx);
    int16_t asy = (int16_t)(sy < 0 ? -sy : sy);
    int16_t w = (int16_t)(r.w - asx), h = (int16_t)(r.h - asy);
    if (w <= 0 || h <= 0)
        return;
    surf_rect src = {
        (int16_t)(r.x + (sx < 0 ? asx : 0)),
        (int16_t)(r.y + (sy < 0 ? asy : 0)),
        w, h,
    };
    cross_copy(S.cfg.scan_fbs[S.last_flip], src,
               (int16_t)(r.x + (sx > 0 ? sx : 0)),
               (int16_t)(r.y + (sy > 0 ? sy : 0)));
    /* DMA wrote behind the cache: invalidate the band so later CPU
     * writes (sliver compose is PPA, but textgrid-style writes could
     * follow) never merge against stale lines */
    uintptr_t lo = (uintptr_t)S.fb + (uintptr_t)r.y * S.cfg.w * 2;
    uintptr_t hi = lo + (uintptr_t)r.h * S.cfg.w * 2;
    lo &= ~(uintptr_t)(P4_ALIGN - 1);
    hi = (hi + P4_ALIGN - 1) & ~(uintptr_t)(P4_ALIGN - 1);
    esp_cache_msync((void *)lo, hi - lo, ESP_CACHE_MSYNC_FLAG_DIR_M2C);
    /* deliberately NOT S.scrolled: the band heals itself every frame */
}

static surf_hal hal_p4 = {
    .fill = h_fill,
    .blit = h_blit,
    .blend = h_blend,
    .xform_blend = h_xform_blend,
    .present = h_present,
    .wait_idle = h_wait_idle,
    .now_us = h_now_us,
    .poll_touch = h_poll_touch,
    .touch_points = h_touch_points,
    .alloc_image = h_alloc_image,
    .free_image = h_free_image,
    .fb_ptr = h_fb_ptr,
    .scroll_rect = h_scroll_rect,
    .band_shift = h_band_shift,
};

const surf_hal *surf_hal_p4_init(const surf_hal_p4_cfg *cfg)
{
    if (!cfg || cfg->w <= 0 || cfg->h <= 0)
        return NULL;
    S.cfg = *cfg;
    S.fb_bytes = (size_t)cfg->w * cfg->h * 2;
    /* DMA2D rect copies back scroll_rect in every mode, and the
     * damage-forward path in triple-buffer mode */
    if (esp_async_fbcpy_install(&(esp_async_fbcpy_config_t){}, &S.fbcpy) != ESP_OK)
        return NULL;
    S.fbcpy_sem = xSemaphoreCreateBinary();
    if (!cfg->scan_fbs[0]) {  /* headless bench */
        S.nfbs = 0;
        S.fb = h_alloc_image(S.fb_bytes);
        if (!S.fb)
            return NULL;
        memset(S.fb, 0, S.fb_bytes);
        surf_hal_p4_sync(S.fb, S.fb_bytes);
    } else if (cfg->single_buffer || !cfg->scan_fbs[1] || !cfg->scan_fbs[2]) {
        S.nfbs = 1;
        S.fb = cfg->scan_fbs[0];
    } else {
        S.nfbs = 3;
        S.live = 0;  /* DSI starts scanning fbs[0] */
        S.last_flip = 0;
        S.back = 1;
        S.fb = cfg->scan_fbs[1];
        esp_lcd_dpi_panel_event_callbacks_t cbs = {.on_refresh_done = vsync_cb};
        if (esp_lcd_dpi_panel_register_event_callbacks(cfg->panel, &cbs, NULL) != ESP_OK)
            return NULL;
    }

    if (S.nfbs == 3 && !S.vsync_sem)
        S.vsync_sem = xSemaphoreCreateBinary();
    S.hz_t0_us = esp_timer_get_time();
    S.hz_c0 = S.vsync_count;

    /* streaming layers need the just-presented buffer as source */
    hal_p4.band_shift = S.nfbs == 3 ? h_band_shift : NULL;
    hal_p4.wait_frame = S.nfbs == 3 ? h_wait_frame : NULL;
    hal_p4.frame_hz = S.nfbs == 3 ? h_frame_hz : NULL;

    ppa_client_config_t fill_c = {.oper_type = PPA_OPERATION_FILL};
    ppa_client_config_t srm_c = {.oper_type = PPA_OPERATION_SRM};
    ppa_client_config_t blend_c = {.oper_type = PPA_OPERATION_BLEND};
    if (ppa_register_client(&fill_c, &S.fill_cl) != ESP_OK ||
        ppa_register_client(&srm_c, &S.srm_cl) != ESP_OK ||
        ppa_register_client(&blend_c, &S.blend_cl) != ESP_OK)
        return NULL;
    return &hal_p4;
}
