/* ESP32-P4 hal: every frame op is a PPA job. fill → fill engine, blit →
 * SRM (1:1, with ARGB8888→RGB565 conversion for free), blend → blend
 * engine (per-pixel fg alpha over RGB565 bg). Blocking transfer mode
 * keeps cross-engine ordering correct; M2 measures whether queueing is
 * ever needed. CPU never touches the compose buffer, so the only cache
 * sync in the system is surf_hal_p4_sync() after asset uploads. */
#include <string.h>

#include "driver/ppa.h"
#include "esp_cache.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"

#include "hal_p4.h"

/* P4 L2 cache line; alignment covers the hal's 64-byte contract too. */
#define P4_ALIGN 128

static struct {
    surf_hal_p4_cfg      cfg;
    uint16_t            *fb;       /* compose buffer, RGB565 */
    size_t               fb_bytes;
    ppa_client_handle_t  fill_cl, srm_cl, blend_cl;
    /* touch edge state */
    bool                 was_down;
    int16_t              last_x, last_y;
    bool                 up_pending;
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
            .pic_w = (uint32_t)(src->stride / (src->format == SURF_FMT_ARGB8888 ? 4 : 2)),
            .pic_h = (uint32_t)src->h,
            .block_w = (uint32_t)sr.w,
            .block_h = (uint32_t)sr.h,
            .block_offset_x = (uint32_t)sr.x,
            .block_offset_y = (uint32_t)sr.y,
            .blend_cm = src->format == SURF_FMT_ARGB8888 ? PPA_BLEND_COLOR_MODE_ARGB8888
                                                         : PPA_BLEND_COLOR_MODE_RGB565,
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
        .mode = PPA_TRANS_MODE_BLOCKING,
    };
    ppa_do_blend(S.blend_cl, &op);
}

static void h_scale_blit(const surf_image *src, surf_rect src_r, surf_rect dst_r)
{
    /* In the vtable per DESIGN.md §5.4; unused by the v1 frame path. */
    (void)src; (void)src_r; (void)dst_r;
}

static void h_present(const surf_rect *dirty, int n)
{
    if (!S.cfg.scan_fb || S.cfg.single_buffer)
        return;
    surf_image fb_img = {
        .pixels = S.fb, .w = S.cfg.w, .h = S.cfg.h,
        .stride = S.cfg.w * 2, .format = SURF_FMT_RGB565, .opaque = true,
    };
    for (int i = 0; i < n; i++)
        srm_copy(&fb_img, dirty[i], S.cfg.scan_fb, (size_t)S.cfg.w * S.cfg.h * 2,
                 S.cfg.w, S.cfg.h, (surf_point){dirty[i].x, dirty[i].y});
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
 * and the DOWN→UP edge is synthesized from the pressed level. */
static bool h_poll_touch(surf_touch *out)
{
    if (!S.cfg.touch_poll)
        return false;

    if (S.up_pending) {
        S.up_pending = false;
        *out = (surf_touch){S.last_x, S.last_y, SURF_TOUCH_UP};
        return true;
    }

    static int64_t last_read;
    int64_t now = esp_timer_get_time();
    if (now - last_read < 8000)
        return false;
    last_read = now;

    int16_t x, y;
    bool down = S.cfg.touch_poll(&x, &y);
    if (down && !S.was_down) {
        S.was_down = true;
        S.last_x = x;
        S.last_y = y;
        *out = (surf_touch){x, y, SURF_TOUCH_DOWN};
        return true;
    }
    if (down && (x != S.last_x || y != S.last_y)) {
        S.last_x = x;
        S.last_y = y;
        *out = (surf_touch){x, y, SURF_TOUCH_MOVE};
        return true;
    }
    if (!down && S.was_down) {
        S.was_down = false;
        *out = (surf_touch){S.last_x, S.last_y, SURF_TOUCH_UP};
        return true;
    }
    return false;
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

void surf_hal_p4_sync(const void *buf, size_t bytes)
{
    esp_cache_msync((void *)buf, (bytes + P4_ALIGN - 1) & ~(size_t)(P4_ALIGN - 1),
                    ESP_CACHE_MSYNC_FLAG_DIR_C2M);
}

static const surf_hal hal_p4 = {
    .fill = h_fill,
    .blit = h_blit,
    .blend = h_blend,
    .scale_blit = h_scale_blit,
    .present = h_present,
    .wait_idle = h_wait_idle,
    .now_us = h_now_us,
    .poll_touch = h_poll_touch,
    .alloc_image = h_alloc_image,
    .free_image = h_free_image,
};

const surf_hal *surf_hal_p4_init(const surf_hal_p4_cfg *cfg)
{
    if (!cfg || cfg->w <= 0 || cfg->h <= 0)
        return NULL;
    S.cfg = *cfg;
    S.fb_bytes = (size_t)cfg->w * cfg->h * 2;
    if (cfg->single_buffer && cfg->scan_fb) {
        S.fb = cfg->scan_fb;
    } else {
        S.fb = h_alloc_image(S.fb_bytes);
        if (!S.fb)
            return NULL;
        memset(S.fb, 0, S.fb_bytes);
        surf_hal_p4_sync(S.fb, S.fb_bytes);
    }

    ppa_client_config_t fill_c = {.oper_type = PPA_OPERATION_FILL};
    ppa_client_config_t srm_c = {.oper_type = PPA_OPERATION_SRM};
    ppa_client_config_t blend_c = {.oper_type = PPA_OPERATION_BLEND};
    if (ppa_register_client(&fill_c, &S.fill_cl) != ESP_OK ||
        ppa_register_client(&srm_c, &S.srm_cl) != ESP_OK ||
        ppa_register_client(&blend_c, &S.blend_cl) != ESP_OK)
        return NULL;
    return &hal_p4;
}
