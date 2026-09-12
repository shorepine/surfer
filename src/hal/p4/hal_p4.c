/* ESP32-P4 hal: every frame op is a PPA job. fill → fill engine, blit →
 * SRM (1:1, with ARGB8888→RGB565 conversion for free), blend → blend
 * engine (per-pixel fg alpha over RGB565 bg). Each op is awaited before
 * the next is submitted, which is what keeps cross-engine ordering
 * correct; the wait is ours and bounded rather than the driver's and
 * unbounded (see ppa_begin/ppa_end). CPU never touches the compose
 * buffer, so the only cache sync in the system is surf_hal_p4_sync()
 * after asset uploads. */
#include <string.h>

#include "driver/ppa.h"
#include "esp_async_fbcpy.h"  /* esp_lcd priv_include: DMA2D rect copy */
#include "esp_attr.h"
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
    bool                 cpu_wrote;      /* fb_ptr handed out since the last writeback */
    uint32_t             frame_no;
    uint32_t             fb_stamp[3];    /* frame each buffer is current to */
    /* rotated scanout (cfg.rotation != 0). `comp` is the compose buffer
     * in LOGICAL orientation — S.fb points at it and everything that
     * draws, reads back or screenshots sees only that, so the rotation
     * exists nowhere above present(). pw/ph are the panel's own raster,
     * swapped from cfg.w/h at 90 and 270. */
    int16_t              rot;
    void                *comp;
    int16_t              pw, ph;
    /* WHAT EACH SCAN BUFFER IS MISSING. The compose buffer is always
     * current, so the only question per buffer is which rects have
     * landed since that buffer was last written — and with three
     * buffers in rotation that is two frames' worth, not one.
     *
     * The first cut leaned on fb_stamp/age the way the unrotated path
     * does and got it wrong in a way that undid the entire point: it
     * stamped only the buffer it wrote, so age was 3 every frame, the
     * `age > 2` arm fired every frame, and every frame did a
     * FULL-SCREEN rotate no matter how little had changed. Measured on
     * the 8.8": damaging 64x64 cost 44.4 ms and damaging 230,400 px
     * cost 44.7 — flat, because the damage was never what was being
     * rotated. */
    surf_rect            acc_r[3][SURF_MAX_DIRTY_P4];
    uint8_t              acc_n[3];
    bool                 acc_over[3];   /* list full: owes everything */
    ppa_client_handle_t  fill_cl, srm_cl, blend_cl;
    SemaphoreHandle_t    ppa_sem;   /* one op in flight; see ppa_begin */
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
    /* per-contact tracking for the multitouch event stream; see
     * h_poll_touch. Parallel to the single-pointer fields above, which
     * the no-multi path still uses. */
    struct {
        bool    down;
        uint8_t id;
        int16_t x, y;
        uint8_t missing;          /* consecutive polls without this id */
        bool    sent_down;
    } contacts[5];
    uint8_t              qn, qr;   /* pending events, ring */
    surf_touch           q[16];
} S;

/* ---- one PPA op, waited for -----------------------------------------
 *
 * Same shape as fbcpy_sync below, same reason, different engine — and
 * this is the one the mirrored-sprite freeze goes through.
 *
 * PPA_TRANS_MODE_BLOCKING waits inside IDF's driver on portMAX_DELAY for
 * a semaphore the 2D-DMA completion ISR gives, and every call site here
 * threw the return code away. IDF says what that costs, in ppa_core.c's
 * ppa_engine_acquire:
 *
 *   // TODO: Register PPA interrupt? Useful for SRM parameter error. If
 *   // SRM parameter error, blocks at 2D-DMA, transaction can never
 *   // finish, stuck... need a way to force end
 *
 * And it is worse than one lost op: the wedged transaction is never
 * removed from its engine's queue, so the engine semaphore is never
 * given back and EVERY later op on that engine blocks behind it. One bad
 * SRM takes the whole compositor with it — panel still scanning out the
 * last frame, REPL dead, ctrl-C ignored, no watchdog (a task blocked on
 * a semaphore is not a CPU spin) and nothing logged.
 *
 * ONE semaphore, given by ONE callback, shared by all three clients: the
 * hal submits an op and waits for it before submitting the next, so
 * there is never more than one in flight and there is nothing to
 * disambiguate. Deliberately NOT routed through the driver's per-op
 * `user_data` — that is the shape a first attempt at this took, and a
 * callback handed the wrong semaphore wakes nobody, which presents not
 * as a hang but as every op sitting out its full timeout: the frame rate
 * collapses while the picture still, confusingly, comes out right.
 *
 * DRAIN BEFORE STARTING, for fbcpy_sync's reason: after a timeout the
 * op may still complete and give, and that give must not satisfy the
 * next wait.
 *
 * ppa_dead LATCHES and is one-way, because there is no public API to
 * reset a stalled SRM engine — once the hardware has stopped completing,
 * every later op would cost a full timeout for nothing and a frame would
 * take minutes. Latched, the picture freezes and THE MACHINE STAYS UP:
 * the REPL answers, ctrl-C works, and the counters below say what
 * happened. That is the whole point — not to fix a wedge, but to turn a
 * dead board into a frozen picture you can ask questions of. */
#define PPA_TIMEOUT_MS 200

uint32_t surf_hal_p4_ppa_timeouts;
uint32_t surf_hal_p4_ppa_errors;
uint32_t surf_hal_p4_ppa_skipped;
bool     surf_hal_p4_ppa_dead;

/* THE OP THAT WEDGED THE ENGINE, for the post-mortem the latch's own
 * comment asks for. Every submit wrapper notes its op's geometry here;
 * the timeout path snapshots it. [0]=kind (0 fill / 1 srm / 2 blend),
 * [1]=in buffer, [2..3]=in pic w/h, [4..5]=block w/h, [6..7]=in
 * offsets, [8..9]=out offsets, [10]=in color mode, [11..12]=per-kind
 * extras (srm: scale_x*1024, rot|mirror bits; blend: alpha mode, fix
 * val), [13]=ops submitted so far. */
uint32_t surf_hal_p4_last_op[14];
uint32_t surf_hal_p4_wedge_op[14];

static void op_note(uint32_t kind, const void *in_buf,
                    uint32_t in_pw, uint32_t in_ph,
                    uint32_t bw, uint32_t bh,
                    uint32_t in_ox, uint32_t in_oy,
                    uint32_t out_ox, uint32_t out_oy,
                    uint32_t cm_in, uint32_t x1, uint32_t x2)
{
    uint32_t *o = surf_hal_p4_last_op;
    o[0] = kind;
    o[1] = (uint32_t)(uintptr_t)in_buf;
    o[2] = in_pw;  o[3] = in_ph;
    o[4] = bw;     o[5] = bh;
    o[6] = in_ox;  o[7] = in_oy;
    o[8] = out_ox; o[9] = out_oy;
    o[10] = cm_in; o[11] = x1; o[12] = x2;
    o[13]++;
}

static bool ppa_done(ppa_client_handle_t c, ppa_event_data_t *ev, void *arg)
{
    (void)c; (void)ev; (void)arg;
    BaseType_t hp = pdFALSE;
    xSemaphoreGiveFromISR(S.ppa_sem, &hp);
    return hp == pdTRUE;
}

static bool ppa_begin(void)
{
    if (surf_hal_p4_ppa_dead || !S.ppa_sem) {
        surf_hal_p4_ppa_skipped++;
        return false;
    }
    xSemaphoreTake(S.ppa_sem, 0);   /* drop a give left by a timed-out op */
    return true;
}

static void ppa_end(esp_err_t submitted)
{
    if (submitted != ESP_OK) {
        /* rejected before the hardware saw it — a bad rect, or a pending
         * slot still held by a wedged transaction. Never a hang. */
        surf_hal_p4_ppa_errors++;
        return;
    }
    if (xSemaphoreTake(S.ppa_sem, pdMS_TO_TICKS(PPA_TIMEOUT_MS)) == pdTRUE)
        return;
    surf_hal_p4_ppa_timeouts++;
    surf_hal_p4_ppa_dead = true;
    memcpy(surf_hal_p4_wedge_op, surf_hal_p4_last_op,
           sizeof(surf_hal_p4_wedge_op));
}

static void ppa_fill_sync(const ppa_fill_oper_config_t *op)
{
    op_note(0, op->out.buffer, op->out.pic_w, op->out.pic_h,
            op->fill_block_w, op->fill_block_h, 0, 0,
            op->out.block_offset_x, op->out.block_offset_y,
            (uint32_t)op->out.fill_cm, 0, 0);
    if (ppa_begin())
        ppa_end(ppa_do_fill(S.fill_cl, op));
}

static void ppa_srm_sync(const ppa_srm_oper_config_t *op)
{
    op_note(1, op->in.buffer, op->in.pic_w, op->in.pic_h,
            op->in.block_w, op->in.block_h,
            op->in.block_offset_x, op->in.block_offset_y,
            op->out.block_offset_x, op->out.block_offset_y,
            (uint32_t)op->in.srm_cm,
            (uint32_t)(op->scale_x * 1024.0f),
            (uint32_t)op->rotation_angle |
                ((uint32_t)op->mirror_x << 8) |
                ((uint32_t)op->mirror_y << 9));
    if (ppa_begin())
        ppa_end(ppa_do_scale_rotate_mirror(S.srm_cl, op));
}

static void ppa_blend_sync(const ppa_blend_oper_config_t *op)
{
    op_note(2, op->in_fg.buffer, op->in_fg.pic_w, op->in_fg.pic_h,
            op->in_fg.block_w, op->in_fg.block_h,
            op->in_fg.block_offset_x, op->in_fg.block_offset_y,
            op->out.block_offset_x, op->out.block_offset_y,
            (uint32_t)op->in_fg.blend_cm,
            (uint32_t)op->fg_alpha_update_mode, op->fg_alpha_fix_val);
    if (ppa_begin())
        ppa_end(ppa_do_blend(S.blend_cl, op));
}

/* THE PPA DRIVER INVALIDATES THE ROWS IT IS ABOUT TO WRITE, AND THE CPU
 * MAY STILL OWN SOME OF THEM. Before an SRM copy or a fill, IDF's
 * driver does an M2C over the output "extended window" -- WHOLE ROWS,
 * pic_w wide, for the block's height -- so any framebuffer pixel the
 * CPU wrote this compose and has not yet written back (a textgrid's
 * cells, through fb_ptr) is dropped from cache before it reaches
 * memory, and memory keeps whatever the forward copy put there: the
 * previous frame. Seen on the glass as a sprite leaving copies of
 * itself wherever an opaque blit shared its rows -- the mouse pointer
 * along the launcher panel's edge, the erase of its old position
 * discarded when the panel's skin was copied on the same rows. A BLEND
 * is safe (the driver writes back in_bg first, and in_bg is this
 * buffer); a COPY or FILL is not. So the rows an op spans are written
 * back first, and only while the CPU has written since the last
 * present -- clean lines cost nothing and a frame with no textgrid in
 * it pays nothing. The blend path is left alone: the driver already
 * does it. */
static void fb_writeback_rows(int32_t y, int32_t h)
{
    if (!S.cpu_wrote || h <= 0)
        return;
    if (y < 0) { h += y; y = 0; }
    if (y + h > S.cfg.h) h = S.cfg.h - y;
    if (h <= 0)
        return;
    uintptr_t lo = (uintptr_t)S.fb + (uintptr_t)y * S.cfg.w * 2;
    uintptr_t hi = lo + (uintptr_t)h * S.cfg.w * 2;
    lo &= ~(uintptr_t)(P4_ALIGN - 1);
    hi = (hi + P4_ALIGN - 1) & ~(uintptr_t)(P4_ALIGN - 1);
    esp_cache_msync((void *)lo, hi - lo, ESP_CACHE_MSYNC_FLAG_DIR_C2M);
}

static void h_fill(surf_rect dst, surf_color c)
{
    fb_writeback_rows(dst.y, dst.h);
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
        .mode = PPA_TRANS_MODE_NON_BLOCKING,
    };
    ppa_fill_sync(&op);
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
        .mode = PPA_TRANS_MODE_NON_BLOCKING,
    };
    ppa_srm_sync(&op);
}

static void h_blit(const surf_image *src, surf_rect sr, surf_point dst)
{
    fb_writeback_rows(dst.y, sr.h);
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
        /* RGB565 carries NO source alpha, so there is nothing for
         * ALPHA_SCALE to scale — it is FIX_VALUE or the fade does
         * nothing. ARGB and A8 have real per-pixel alpha and must be
         * SCALEd, or a node opacity would flatten the art's own
         * transparency to a constant and square off every sprite. */
        .fg_alpha_update_mode =
            opa == 255            ? PPA_ALPHA_NO_CHANGE
            : src->format == SURF_FMT_RGB565 ? PPA_ALPHA_FIX_VALUE
                                             : PPA_ALPHA_SCALE,
        .fg_alpha_fix_val = opa,
        .fg_alpha_scale_ratio = (float)opa / 255.0f,
        /* A8 (glyph atlases): alpha from the data, color from the tint */
        .fg_fix_rgb_val = {
            .r = (uint8_t)(((src->tint >> 11) << 3) | ((src->tint >> 13) & 0x7)),
            .g = (uint8_t)(((src->tint >> 5) << 2) & 0xfc),
            .b = (uint8_t)((src->tint << 3) & 0xf8),
        },
        .mode = PPA_TRANS_MODE_NON_BLOCKING,
    };
    ppa_blend_sync(&op);
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
                          surf_rect vis, uint8_t rot, uint8_t mirror,
                          uint8_t opa)
{
    if (opa == 0)
        return;
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
        .mode = PPA_TRANS_MODE_NON_BLOCKING,
    };
    ppa_fill_sync(&clr);

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
        .mode = PPA_TRANS_MODE_NON_BLOCKING,
    };
    ppa_srm_sync(&srm);

    surf_image scratch = {
        .pixels = S_xform.buf,
        .w = dst_r.w,
        .h = dst_r.h,
        .stride = stride,
        .format = src->format,
        .opaque = src->opaque,
        .tint = src->tint,
    };
    /* opa < 255 forces the blend engine even for an opaque source: the
     * PPA's fg_alpha_scale_ratio is where the fade actually happens, and
     * h_blit has no such field. Same two ops either way. */
    if (src->opaque && opa == 255)
        h_blit(&scratch,
               (surf_rect){(int16_t)(vis.x - dst_r.x), (int16_t)(vis.y - dst_r.y),
                           vis.w, vis.h},
               (surf_point){vis.x, vis.y});
    else
        h_blend(&scratch,
                (surf_rect){(int16_t)(vis.x - dst_r.x), (int16_t)(vis.y - dst_r.y),
                            vis.w, vis.h},
                (surf_point){vis.x, vis.y}, opa);
}

static bool fbcpy_done(esp_async_fbcpy_handle_t mcp, esp_async_fbcpy_event_data_t *ev,
                       void *arg)
{
    (void)mcp; (void)ev;
    BaseType_t hp = pdFALSE;
    xSemaphoreGiveFromISR((SemaphoreHandle_t)arg, &hp);
    return hp == pdTRUE;
}


/* ---- one DMA2D copy, waited for -------------------------------------
 *
 * **THE SEMAPHORE IS BINARY, AND THAT IS THE HAZARD.** `fbcpy_done`
 * gives it from the completion ISR; a give onto an already-full binary
 * semaphore is DISCARDED, not counted. So if the handshake ever drifts
 * by one — a give landing with no take waiting — the next give is lost
 * and the take after it waits on something that will never arrive. With
 * portMAX_DELAY that is not a dropped frame, it is a dead machine: the
 * compositor blocks inside C and the panel keeps scanning out the last
 * framebuffer.
 *
 * Two changes, and the first is the actual fix:
 *
 *   DRAIN BEFORE STARTING. A stale give from a previous transfer would
 *   otherwise satisfy THIS wait immediately, handing the caller a buffer
 *   that DMA2D is still writing into — and leaving the drift in place to
 *   strand a later frame.
 *
 *   BOUND THE WAIT. The vsync take beside this one has always had a
 *   40 ms timeout; these three had none. A copy that has not landed in
 *   100 ms is not going to, and a torn frame beats a hang.
 *
 * Found while chasing a different freeze (a mirrored sprite wedging the
 * PPA SRM path) — this is NOT that bug and does not fix it. It is a
 * latent hazard on its own and is fixed here on its own merits.
 *
 * ...AND THEN IT LATCHES, because bounding the wait was only half of
 * it. `ppa_dead` gives up after the first timeout and skips every later
 * op; this path did not, and that asymmetry is a whole bug on its own:
 * the two share the 2D-DMA, so a wedged PPA transaction that never
 * leaves the engine's queue takes the COPIES down with it, and an
 * unlatched copy pays its full timeout for ever.
 *
 * Measured on a P4 with a real app, the frame it happens on and every
 * frame after:
 *
 *   frame 276  compose 502 ms  ppa_timeouts 0->1  ppa_dead 0->1  fbcpy 3
 *   frame 277  compose 302 ms  ppa_skipped +14/f  fbcpy_timeouts +3/f
 *   frame 296  compose 302 ms  ...unchanged, for ever
 *
 * and the arithmetic is exactly this function: THREE copies a frame at
 * 100 ms each is the 300 ms, and the first frame's 502 is that plus the
 * one 200 ms PPA timeout that started it. The picture was frozen either
 * way; what the missing latch bought was a machine at 3 fps instead of
 * a machine that still answers — which defeats the whole point stated
 * above ppa_dead, "not to fix a wedge, but to turn a dead board into a
 * frozen picture you can ask questions of". One of the two paths
 * honoured that and the other quietly did not.
 *
 * ONE timeout latches, ppa_dead's rule and for ppa_dead's reason: there
 * is no API to reset a stalled 2D-DMA engine, so a second attempt is
 * 100 ms spent to learn what the first one already proved. A copy that
 * has not landed in 100 ms is not a busy machine, it is a lost
 * completion.
 */
#define FBCPY_TIMEOUT_MS 100

/* copies that gave up waiting; 0 on a healthy machine */
uint32_t surf_hal_p4_fbcpy_timeouts;
bool     surf_hal_p4_fbcpy_dead;

static void fbcpy_sync(const esp_async_fbcpy_trans_desc_t *t)
{
    if (!S.fbcpy || surf_hal_p4_fbcpy_dead)
        return;
    xSemaphoreTake(S.fbcpy_sem, 0);          /* discard any stale give */
    if (esp_async_fbcpy(S.fbcpy, (esp_async_fbcpy_trans_desc_t *)t,
                        fbcpy_done, S.fbcpy_sem) != ESP_OK)
        return;
    if (xSemaphoreTake(S.fbcpy_sem, pdMS_TO_TICKS(FBCPY_TIMEOUT_MS)) != pdTRUE) {
        surf_hal_p4_fbcpy_timeouts++;
        surf_hal_p4_fbcpy_dead = true;
    }
}

/* The end-of-frame ISR is when the DPI DMA latches the most recently
 * flipped buffer; recording it tells present which buffer is on glass.
 * IRAM, because a host may build with CONFIG_LCD_DSI_ISR_CACHE_SAFE so
 * the panel keeps refreshing through a flash erase (the DPI driver
 * restarts its frame DMA from an ISR, and with the cache off that ISR
 * is masked and the glass goes blank for the length of the erase) --
 * and under that option the driver refuses a callback outside IRAM.
 * Everything this touches is a static in DRAM and a FreeRTOS give. */
static bool IRAM_ATTR vsync_cb(esp_lcd_panel_handle_t panel, esp_lcd_dpi_panel_event_data_t *ev,
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
    fbcpy_sync(&t);
}

/* ---- rotated present ------------------------------------------------
 *
 * One dirty rect out of the compose buffer and into a scan buffer, turned
 * on the way by the SRM engine. The arithmetic is the whole of it, and it
 * is worth writing down because getting it 180 degrees wrong still draws
 * a picture:
 *
 * PPA rotates COUNTER-CLOCKWISE. At 90 the source's top-right corner
 * becomes the destination's top-left, so a logical (x, y) lands at
 * (y, W-1-x) and a block of (w, h) comes out (h, w). At 270 it is
 * (H-1-y, x), and at 180 the plain (W-1-x, H-1-y).
 *
 * `S.comp` is CPU-written in places (textgrid), and the PPA reads
 * physical memory — so the caller writes back before the first rect of a
 * frame, not once per rect. */
static void rot_copy(int dst_fb, surf_rect r)
{
    const int16_t W = S.cfg.w, H = S.cfg.h;
    uint32_t angle;
    int32_t dx, dy;

    switch (S.rot) {
    case 90:
        angle = PPA_SRM_ROTATION_ANGLE_90;
        dx = r.y;
        dy = W - r.x - r.w;
        break;
    case 180:
        angle = PPA_SRM_ROTATION_ANGLE_180;
        dx = W - r.x - r.w;
        dy = H - r.y - r.h;
        break;
    default: /* 270 */
        angle = PPA_SRM_ROTATION_ANGLE_270;
        dx = H - r.y - r.h;
        dy = r.x;
        break;
    }

    ppa_srm_oper_config_t op = {
        .in = {
            .buffer = S.comp,
            .pic_w = (uint32_t)W,
            .pic_h = (uint32_t)H,
            .block_w = (uint32_t)r.w,
            .block_h = (uint32_t)r.h,
            .block_offset_x = (uint32_t)r.x,
            .block_offset_y = (uint32_t)r.y,
            .srm_cm = PPA_SRM_COLOR_MODE_RGB565,
        },
        .out = {
            .buffer = S.cfg.scan_fbs[dst_fb],
            .buffer_size = (size_t)S.pw * S.ph * 2,
            .pic_w = (uint32_t)S.pw,
            .pic_h = (uint32_t)S.ph,
            .block_offset_x = (uint32_t)dx,
            .block_offset_y = (uint32_t)dy,
            .srm_cm = PPA_SRM_COLOR_MODE_RGB565,
        },
        .rotation_angle = angle,
        .scale_x = 1.0f,
        .scale_y = 1.0f,
        .mode = PPA_TRANS_MODE_NON_BLOCKING,
    };
    ppa_srm_sync(&op);
}

/* ROTATE, THEN FLIP — the order is forced, not chosen. Unrotated, present
 * flips to the buffer just composed and only then brings the NEXT one up
 * to date, so the copy is off the critical path. Here nothing has been
 * composed into a scan buffer at all: the frame has to be turned into one
 * before there is anything to show.
 *
 * What that buys back is the bookkeeping. Unrotated, a stale rect is
 * fetched from whichever scan buffer happens to be newest; here the
 * source is always `comp`, which is always current — so the only thing
 * to track is WHICH RECTS each scan buffer has not seen yet. Every
 * frame's damage is owed to the two buffers we are not writing, and the
 * one we are writing is brought current from its own list and then
 * cleared.
 *
 * A list per buffer and not a bounding union, which was the cheap
 * version and is wrong on this machine specifically: the console is at
 * the top and the task bar's clock is at the bottom, so the union of a
 * typed character and a ticking minute is the whole screen. Overflowing
 * the list is the only thing that falls back to a full rotate. */
static bool acc_overlap(surf_rect a, surf_rect b)
{
    return a.x < b.x + b.w && b.x < a.x + a.w &&
           a.y < b.y + b.h && b.y < a.y + a.h;
}

static surf_rect acc_union(surf_rect a, surf_rect b)
{
    int16_t x0 = a.x < b.x ? a.x : b.x;
    int16_t y0 = a.y < b.y ? a.y : b.y;
    int16_t x1 = a.x + a.w > b.x + b.w ? (int16_t)(a.x + a.w) : (int16_t)(b.x + b.w);
    int16_t y1 = a.y + a.h > b.y + b.h ? (int16_t)(a.y + a.h) : (int16_t)(b.y + b.h);
    return (surf_rect){x0, y0, (int16_t)(x1 - x0), (int16_t)(y1 - y0)};
}

/* MERGE ON INSERT, surf_dirty_add's own rule and for a sharper reason
 * here. Without it a full-screen repaint every frame put the SAME whole
 * screen into each buffer's list once per frame it was not chosen, and
 * the chosen buffer then rotated three of them: measured 135 ms against
 * the 44 one rotate costs. Rotating a rect twice is only wasted work,
 * but wasting it three times over the whole panel is the difference
 * between 7 fps and 22.
 *
 * The grown rect may newly overlap others, so it repeats until it
 * settles — a list of touching rects has to collapse to one. */
static void acc_add(int b, surf_rect r)
{
    if (S.acc_over[b])
        return;                       /* already owes everything */
    bool merged = true;
    while (merged) {
        merged = false;
        for (int i = 0; i < S.acc_n[b]; i++) {
            if (acc_overlap(S.acc_r[b][i], r)) {
                r = acc_union(S.acc_r[b][i], r);
                S.acc_r[b][i] = S.acc_r[b][--S.acc_n[b]];
                merged = true;
                break;
            }
        }
    }
    if (S.acc_n[b] >= SURF_MAX_DIRTY_P4) {
        S.acc_over[b] = true;
        return;
    }
    S.acc_r[b][S.acc_n[b]++] = r;
}

static void present_rotated(const surf_rect *dirty, int n)
{
    uint8_t live = S.live;            /* snapshot: the ISR moves it */
    int back = -1;
    for (int i = 0; i < 3; i++) {
        if (i != live && i != S.last_flip) {
            back = i;
            break;
        }
    }
    if (back < 0)
        back = (S.last_flip + 1) % 3;

    /* CPU writes reach physical memory once per frame, not once per rect
     * — and over the DAMAGED ROWS, not the whole buffer. The first cut
     * wrote back all of it: 1.84 MB and 14,400 cache lines every frame,
     * to publish what is usually a few hundred rows of a console. PPA
     * and DMA2D content is not cached at all, so the only thing this has
     * to catch is a CPU write (a textgrid), and those live inside the
     * damage like everything else. */
    int miny = S.cfg.h, maxy = 0;
    for (int i = 0; i < n; i++) {
        if (dirty[i].y < miny) miny = dirty[i].y;
        if (dirty[i].y + dirty[i].h > maxy) maxy = dirty[i].y + dirty[i].h;
    }
    if (maxy > miny)
        surf_hal_p4_sync((uint8_t *)S.comp + (size_t)miny * S.cfg.w * 2,
                         (size_t)(maxy - miny) * S.cfg.w * 2);
    S.cpu_wrote = false;

    /* scroll_rect moved pixels outside the damage system's view, so no
     * buffer can be trusted rect-by-rect — including this one. */
    if (S.scrolled) {
        S.scrolled = false;
        S.shift_acc = 0;
        for (int i = 0; i < 3; i++)
            S.acc_over[i] = true;
    }

    /* EVERY buffer owes this frame, the one we are about to write
     * included — so that what it missed and what just happened coalesce
     * into ONE list before any of it is rotated, rather than being two
     * lists rotated back to back over the same pixels. */
    for (int i = 0; i < 3; i++)
        for (int j = 0; j < n; j++)
            acc_add(i, dirty[j]);

    if (S.acc_over[back]) {
        surf_rect full = {0, 0, S.cfg.w, S.cfg.h};
        rot_copy(back, full);
    } else {
        for (int j = 0; j < S.acc_n[back]; j++)
            rot_copy(back, S.acc_r[back][j]);
    }
    S.acc_n[back] = 0;
    S.acc_over[back] = false;

    esp_lcd_panel_draw_bitmap(S.cfg.panel, 0, 0, S.pw, S.ph,
                              S.cfg.scan_fbs[back]);
    S.last_flip = (uint8_t)back;
    /* S.fb never moves: compose always goes to the one logical buffer */
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

    if (S.rot) {
        present_rotated(dirty, n);
        return;
    }

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
        S.cpu_wrote = false;
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
    S.cpu_wrote = false;         /* everything the CPU wrote is in memory now */
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
    /* Nothing to wait for: every op is awaited at its own call site, so
     * the engines are already idle whenever core code can ask. */
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

/* One event at a time out of a queue we refill from the controller.
 *
 * MULTITOUCH: every contact gets its own DOWN/MOVE/UP stream, keyed by
 * the controller's track id, because dispatch captures per contact now
 * (src/core/input.c) and three fingers on three faders is three drags.
 * This used to synthesise ONE pointer from `s_pts[0]`, which was wrong
 * twice over: it threw four fingers away, and s_pts[0] is not even a
 * stable finger — lift the first of two and the remaining one shuffles
 * down into slot 0, so the single pointer would TELEPORT across the
 * screen mid-gesture rather than reporting an UP and a MOVE.
 *
 * The release hysteresis is per contact and stays for the reason it was
 * added: the GT911 blinks a contact out for a poll or two when a finger
 * rolls or lifts, and declaring UP on the first empty read synthesised a
 * phantom second tap — visible as a toggle button flipping twice. */
static void queue_event(int16_t x, int16_t y, uint8_t phase, uint8_t id)
{
    if (S.qn >= (uint8_t)(sizeof S.q / sizeof S.q[0]))
        return;                     /* full: drop, rather than corrupt */
    uint8_t w = (uint8_t)((S.qr + S.qn) % (sizeof S.q / sizeof S.q[0]));
    S.q[w] = (surf_touch){x, y, phase, id};
    S.qn++;
}

static void refill_multi(void)
{
    int n = S.cfg.touch_poll_multi(s_pts, P4_MAX_CONTACTS);
    if (n < 0)
        return;                     /* no fresh report: hold everything */
    s_npts = n;

    /* which tracked contacts are still present */
    bool seen[5] = {false, false, false, false, false};
    for (int i = 0; i < n; i++) {
        int slot = -1;
        for (int k = 0; k < 5; k++)
            if (S.contacts[k].down && S.contacts[k].id == s_pts[i].id)
                slot = k;
        if (slot < 0) {
            for (int k = 0; k < 5; k++)
                if (!S.contacts[k].down) { slot = k; break; }
            if (slot < 0)
                continue;           /* more fingers than we follow */
            S.contacts[slot].down = true;
            S.contacts[slot].id = s_pts[i].id;
            S.contacts[slot].sent_down = false;
        }
        seen[slot] = true;
        S.contacts[slot].missing = 0;
        if (!S.contacts[slot].sent_down) {
            S.contacts[slot].sent_down = true;
            S.contacts[slot].x = s_pts[i].x;
            S.contacts[slot].y = s_pts[i].y;
            queue_event(s_pts[i].x, s_pts[i].y, SURF_TOUCH_DOWN,
                        S.contacts[slot].id);
        } else if (s_pts[i].x != S.contacts[slot].x ||
                   s_pts[i].y != S.contacts[slot].y) {
            S.contacts[slot].x = s_pts[i].x;
            S.contacts[slot].y = s_pts[i].y;
            queue_event(s_pts[i].x, s_pts[i].y, SURF_TOUCH_MOVE,
                        S.contacts[slot].id);
        }
    }
    for (int k = 0; k < 5; k++) {
        if (!S.contacts[k].down || seen[k])
            continue;
        if (++S.contacts[k].missing < TOUCH_RELEASE_POLLS)
            continue;               /* a blink, not a lift */
        S.contacts[k].down = false;
        S.contacts[k].missing = 0;
        if (S.contacts[k].sent_down)
            queue_event(S.contacts[k].x, S.contacts[k].y, SURF_TOUCH_UP,
                        S.contacts[k].id);
    }
}

static bool h_poll_touch(surf_touch *out)
{
    if (!S.cfg.touch_poll && !S.cfg.touch_poll_multi)
        return false;

    if (S.qn) {                     /* drain what the last read produced */
        *out = S.q[S.qr];
        S.qr = (uint8_t)((S.qr + 1) % (sizeof S.q / sizeof S.q[0]));
        S.qn--;
        return true;
    }

    static int64_t last_read;
    int64_t now = esp_timer_get_time();
    if (now - last_read < 8000)
        return false;
    last_read = now;

    if (S.cfg.touch_poll_multi) {
        refill_multi();
        if (!S.qn)
            return false;
        *out = S.q[S.qr];
        S.qr = (uint8_t)((S.qr + 1) % (sizeof S.q / sizeof S.q[0]));
        S.qn--;
        return true;
    }

    /* single-pointer controller: contact 0, exactly as before */
    int16_t x = 0, y = 0;
    bool down = S.cfg.touch_poll(&x, &y);
    s_npts = down ? 1 : 0;
    if (down)
        s_pts[0] = (surf_touch_pt){x, y, 0};
    if (down) {
        S.empty_reads = 0;
        if (!S.was_down) {
            S.was_down = true;
            S.last_x = x;
            S.last_y = y;
            *out = (surf_touch){x, y, SURF_TOUCH_DOWN, 0};
            return true;
        }
        if (x != S.last_x || y != S.last_y) {
            S.last_x = x;
            S.last_y = y;
            *out = (surf_touch){x, y, SURF_TOUCH_MOVE, 0};
            return true;
        }
        return false;
    }
    if (S.was_down && ++S.empty_reads >= TOUCH_RELEASE_POLLS) {
        S.was_down = false;
        S.empty_reads = 0;
        *out = (surf_touch){S.last_x, S.last_y, SURF_TOUCH_UP, 0};
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
    S.cpu_wrote = true;          /* fb_writeback_rows: a PPA op must not drop these */
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
    fbcpy_sync(&t);
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
    fbcpy_sync(&t);
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

    /* !S.rot: the cross-buffer source is the last SCANNED frame, which
     * under rotation is not in the orientation this shift is expressed
     * in. Fall through to the in-place strip shift, which moves pixels
     * inside the compose buffer and is orientation-agnostic. */
    if (S.nfbs == 3 && !S.rot) {
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

/* Image memory the CPU writes fast: INTERNAL SRAM rather than the PSRAM
 * h_alloc_image hands out. For the one image an emulator or a decoder
 * renders into every frame -- see surf_image_new_fast.
 *
 * MALLOC_CAP_DMA as well as INTERNAL, and that is the load-bearing bit:
 * the PPA reads an image by DMA, so memory the CPU can reach but the
 * blitter cannot would draw nothing at all. Asking for both means the
 * allocator answers NULL rather than handing back something unusable,
 * and NULL is a documented, harmless outcome -- image_new falls back.
 *
 * Internal SRAM is ~768 KB with a whole machine already living in it, so
 * this WILL fail for anything large and is expected to. */
static void *h_alloc_image_fast(size_t bytes)
{
    /* DMA-CAPABLE internal, and nothing looser: the PPA reads an image by
     * DMA, so memory the CPU can reach and the blitter cannot would draw
     * nothing. NULL is the right answer when there is none.
     *
     * MEASURED ON THE P4X, and the numbers say what this can and cannot
     * do: with the machine up there is ~49 KB of DMA-capable internal
     * free. The largest internal block is 139 KB but it is IRAM -- IDF
     * reports it as executable, it is not byte-addressable, and an image
     * indexed per byte cannot live there. So anything screen-sized falls
     * back today and a NES frame (114 KB) certainly does; a scope trace
     * or a small render target fits. Do not "fix" this by dropping the
     * DMA cap -- that was tried, and it still fails for the same 8-bit
     * reason, having first risked handing out memory the PPA cannot
     * read. */
    return heap_caps_aligned_alloc(P4_ALIGN,
                                   (bytes + P4_ALIGN - 1) & ~(size_t)(P4_ALIGN - 1),
                                   MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA);
}

/* hal->scale_image: one whole image into another, on the PPA.
 *
 * The op the SRM block was built for, pointed at two images instead of
 * at the framebuffer -- so a downscale-then-upscale (i.e. a wide blur)
 * costs two DMA passes and no CPU at all.
 *
 * THE CACHE IS THE WHOLE DIFFICULTY, and it goes both ways. The PPA
 * reads and writes PHYSICAL memory while the CPU works through a cache,
 * so src has to be written BACK before the engine reads it and dst has
 * to be INVALIDATED after, or the CPU reads a stale copy of pixels the
 * PPA has already replaced. Getting either wrong looks like a shader bug
 * -- a frame late, or torn -- and only on the device.
 *
 * Returns false rather than guessing for formats the block does not do,
 * or a scale factor outside what it accepts; the caller has a CPU path. */
static bool h_scale_image(surf_image *dst, const surf_image *src)
{
    if (dst->format != src->format)
        return false;
    if (src->format != SURF_FMT_RGB565 && src->format != SURF_FMT_ARGB8888)
        return false;

    int bpp = src->format == SURF_FMT_ARGB8888 ? 4 : 2;
    float sx = (float)dst->w / (float)src->w;
    float sy = (float)dst->h / (float)src->h;
    /* the SRM block's own limits; outside them it would silently do
     * something else, so hand it back to the CPU instead */
    if (sx < 0.0625f || sx > 16.0f || sy < 0.0625f || sy > 16.0f)
        return false;

    surf_hal_p4_sync(src->pixels, (size_t)src->stride * src->h);

    ppa_srm_oper_config_t op = {
        .in = {
            .buffer = src->pixels,
            .pic_w = (uint32_t)(src->stride / bpp),
            .pic_h = (uint32_t)src->h,
            .block_w = (uint32_t)src->w,
            .block_h = (uint32_t)src->h,
            .block_offset_x = 0,
            .block_offset_y = 0,
            .srm_cm = srm_cm(src),
        },
        .out = {
            .buffer = dst->pixels,
            .buffer_size = (uint32_t)((size_t)dst->stride * dst->h),
            .pic_w = (uint32_t)(dst->stride / bpp),
            .pic_h = (uint32_t)dst->h,
            .block_offset_x = 0,
            .block_offset_y = 0,
            .srm_cm = srm_cm(src),
        },
        .rotation_angle = PPA_SRM_ROTATION_ANGLE_0,
        .scale_x = sx,
        .scale_y = sy,
        .mirror_x = false,
        .mirror_y = false,
        .mode = PPA_TRANS_MODE_NON_BLOCKING,
    };
    ppa_srm_sync(&op);

    /* the engine wrote it; drop whatever the CPU still thinks is there */
    esp_cache_msync(dst->pixels,
                    ((size_t)dst->stride * dst->h + P4_ALIGN - 1) &
                        ~(size_t)(P4_ALIGN - 1),
                    ESP_CACHE_MSYNC_FLAG_DIR_M2C);
    return true;
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

/* hal->sync_image: the same writeback, reached generically. The header note
 * above says the only cache sync in the system is after asset uploads, and
 * that was true while every image was written once. Anything rendering INTO
 * an image per frame (a scope, a video decoder, an emulator) writes through
 * the cache and the PPA reads physical memory, so it has to land first. */
static void h_sync_image(const void *buf, size_t bytes)
{
    surf_hal_p4_sync(buf, bytes);
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
    .alloc_image_fast = h_alloc_image_fast,
    .scale_image = h_scale_image,
    .free_image = h_free_image,
    .sync_image = h_sync_image,
    .fb_ptr = h_fb_ptr,
    .scroll_rect = h_scroll_rect,
    .band_shift = h_band_shift,
};

const surf_hal *surf_hal_p4_init(const surf_hal_p4_cfg *cfg)
{
    if (!cfg || cfg->w <= 0 || cfg->h <= 0)
        return NULL;
    if (cfg->rotation != 0 && cfg->rotation != 90 && cfg->rotation != 180 &&
        cfg->rotation != 270)
        return NULL;
    S.cfg = *cfg;
    S.fb_bytes = (size_t)cfg->w * cfg->h * 2;
    /* A rotation needs somewhere to compose that is not a scan buffer, so
     * it needs all three of them free to be written into — and it needs
     * the flip, which single-buffer has not got. */
    S.rot = (cfg->panel && !cfg->single_buffer && cfg->scan_fbs[0] &&
             cfg->scan_fbs[1] && cfg->scan_fbs[2]) ? cfg->rotation : 0;
    S.pw = (S.rot == 90 || S.rot == 270) ? cfg->h : cfg->w;
    S.ph = (S.rot == 90 || S.rot == 270) ? cfg->w : cfg->h;
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
        if (S.rot) {
            /* compose off to the side, in logical orientation, and never
             * move: with one always-current source there is no buffer to
             * bring up to date but the scanout's own. */
            S.comp = h_alloc_image(S.fb_bytes);
            if (!S.comp)
                return NULL;
            memset(S.comp, 0, S.fb_bytes);
            surf_hal_p4_sync(S.comp, S.fb_bytes);
            S.fb = S.comp;
            /* nothing has been rotated into any of them yet, and what
             * they hold is whatever the allocator left there */
            for (int i = 0; i < 3; i++) {
                S.acc_n[i] = 0;
                S.acc_over[i] = true;
            }
        }
        esp_lcd_dpi_panel_event_callbacks_t cbs = {.on_refresh_done = vsync_cb};
        if (esp_lcd_dpi_panel_register_event_callbacks(cfg->panel, &cbs, NULL) != ESP_OK)
            return NULL;
    }

    if (S.nfbs == 3 && !S.vsync_sem)
        S.vsync_sem = xSemaphoreCreateBinary();
    S.hz_t0_us = esp_timer_get_time();
    S.hz_c0 = S.vsync_count;

    /* streaming layers need the just-presented buffer as source */
    /* ...and NOT while rotating: a band shift copies from the last
     * scanned frame, which is turned. Nulling it is how a layer falls
     * back to repainting, exactly as it does in single-buffer mode. */
    hal_p4.band_shift = (S.nfbs == 3 && !S.rot) ? h_band_shift : NULL;
    hal_p4.wait_frame = S.nfbs == 3 ? h_wait_frame : NULL;
    hal_p4.frame_hz = S.nfbs == 3 ? h_frame_hz : NULL;

    ppa_client_config_t fill_c = {.oper_type = PPA_OPERATION_FILL};
    ppa_client_config_t srm_c = {.oper_type = PPA_OPERATION_SRM};
    ppa_client_config_t blend_c = {.oper_type = PPA_OPERATION_BLEND};
    if (ppa_register_client(&fill_c, &S.fill_cl) != ESP_OK ||
        ppa_register_client(&srm_c, &S.srm_cl) != ESP_OK ||
        ppa_register_client(&blend_c, &S.blend_cl) != ESP_OK)
        return NULL;

    /* the completion signal our own bounded wait rides on. max_pending
     * stays at the default 1: the hal never has two ops in flight, and a
     * client whose slot is still held by a wedged transaction then fails
     * its next submit outright instead of queueing behind it. */
    S.ppa_sem = xSemaphoreCreateBinary();
    if (!S.ppa_sem)
        return NULL;
    ppa_event_callbacks_t ppa_cbs = {.on_trans_done = ppa_done};
    if (ppa_client_register_event_callbacks(S.fill_cl, &ppa_cbs) != ESP_OK ||
        ppa_client_register_event_callbacks(S.srm_cl, &ppa_cbs) != ESP_OK ||
        ppa_client_register_event_callbacks(S.blend_cl, &ppa_cbs) != ESP_OK)
        return NULL;
    return &hal_p4;
}
