/* SDL2 backend: RGB565 software framebuffer + streaming texture. This file
 * is the only place outside build tools where per-pixel loops are allowed;
 * on device the same ops are PPA/2D-DMA jobs. */
#include <SDL.h>
#include <stdlib.h>
#include <string.h>
#if defined(__EMSCRIPTEN__) && !defined(SURF_HAL_SDL_NO_YIELD)
#include <emscripten.h>
EM_ASYNC_JS(void, surf_web_raf_yield, (void), {
    await new Promise((resolve) => requestAnimationFrame(resolve));
});
#endif

#include "hal_sdl.h"

#define TOUCH_RING 32

static struct {
    SDL_Window   *win;
    SDL_Renderer *ren;
    SDL_Texture  *tex;
    uint16_t     *fb;
    int16_t       w, h;
    surf_touch    ring[TOUCH_RING];
    int           ring_r, ring_w;
    surf_sdl_key  keys[TOUCH_RING];
    int           key_r, key_w;
    bool          mouse_down;
    surf_rect     scrolled;     /* union of scroll_rect regions this frame */
    bool          has_scrolled;
} S;

static void push_key(uint8_t kind, bool shift, const char *utf8)
{
    int next = (S.key_w + 1) % TOUCH_RING;
    if (next == S.key_r)
        return;
    surf_sdl_key *k = &S.keys[S.key_w];
    k->kind = kind;
    k->shift = shift;
    k->utf8[0] = 0;
    if (utf8) {
        strncpy(k->utf8, utf8, sizeof k->utf8 - 1);
        k->utf8[sizeof k->utf8 - 1] = 0;
    }
    S.key_w = next;
}

bool surf_hal_sdl_poll_key(surf_sdl_key *out)
{
    if (S.key_r == S.key_w)
        return false;
    *out = S.keys[S.key_r];
    S.key_r = (S.key_r + 1) % TOUCH_RING;
    return true;
}

/* ---- frame ops ---- */

static void h_fill(surf_rect dst, surf_color c)
{
    for (int y = 0; y < dst.h; y++) {
        uint16_t *row = S.fb + (dst.y + y) * S.w + dst.x;
        for (int x = 0; x < dst.w; x++)
            row[x] = c;
    }
}

static inline uint16_t argb_to_565(uint32_t p)
{
    return (uint16_t)(((p >> 8) & 0xf800) | ((p >> 5) & 0x07e0) | ((p >> 3) & 0x001f));
}

static void h_blit(const surf_image *src, surf_rect sr, surf_point dst)
{
    if (src->format == SURF_FMT_RGB565) {
        for (int y = 0; y < sr.h; y++) {
            const uint16_t *s =
                (const uint16_t *)((const uint8_t *)src->pixels + (sr.y + y) * src->stride) + sr.x;
            memcpy(S.fb + (dst.y + y) * S.w + dst.x, s, (size_t)sr.w * 2);
        }
        return;
    }
    for (int y = 0; y < sr.h; y++) {
        const uint32_t *s =
            (const uint32_t *)((const uint8_t *)src->pixels + (sr.y + y) * src->stride) + sr.x;
        uint16_t *d = S.fb + (dst.y + y) * S.w + dst.x;
        for (int x = 0; x < sr.w; x++)
            d[x] = argb_to_565(s[x]);
    }
}

static inline uint16_t blend_px(uint16_t dst, uint32_t src, uint32_t a)
{
    if (a == 0)
        return dst;
    uint32_t sr = (src >> 16) & 0xff, sg = (src >> 8) & 0xff, sb = src & 0xff;
    uint32_t dr = (uint32_t)((dst >> 8) & 0xf8) | (dst >> 13);
    uint32_t dg = (uint32_t)((dst >> 3) & 0xfc) | ((dst >> 9) & 0x03);
    uint32_t db = (uint32_t)((dst << 3) & 0xf8) | ((dst >> 2) & 0x07);
    uint32_t r = (sr * a + dr * (255 - a) + 127) / 255;
    uint32_t g = (sg * a + dg * (255 - a) + 127) / 255;
    uint32_t b = (sb * a + db * (255 - a) + 127) / 255;
    return (uint16_t)(((r & 0xf8) << 8) | ((g & 0xfc) << 3) | (b >> 3));
}

static void h_blend(const surf_image *src, surf_rect sr, surf_point dst, uint8_t opa)
{
    if (src->format == SURF_FMT_A8) {
        /* glyph atlases: alpha from the image, color from the tint */
        surf_color t = src->tint;
        uint32_t rgb = ((uint32_t)((t >> 8) & 0xf8) << 16) |
                       ((uint32_t)((t >> 3) & 0xfc) << 8) |
                       (uint32_t)((t << 3) & 0xf8);
        for (int y = 0; y < sr.h; y++) {
            const uint8_t *s = (const uint8_t *)src->pixels + (sr.y + y) * src->stride + sr.x;
            uint16_t *d = S.fb + (dst.y + y) * S.w + dst.x;
            for (int x = 0; x < sr.w; x++)
                d[x] = blend_px(d[x], rgb, (uint32_t)s[x] * opa / 255);
        }
        return;
    }
    if (src->format == SURF_FMT_RGB565) {
        /* 565 has no per-pixel alpha; only the global opacity applies. */
        if (opa == 255) {
            h_blit(src, sr, dst);
            return;
        }
        for (int y = 0; y < sr.h; y++) {
            const uint16_t *s =
                (const uint16_t *)((const uint8_t *)src->pixels + (sr.y + y) * src->stride) + sr.x;
            uint16_t *d = S.fb + (dst.y + y) * S.w + dst.x;
            for (int x = 0; x < sr.w; x++) {
                uint32_t p = ((uint32_t)((s[x] >> 8) & 0xf8) << 16) |
                             ((uint32_t)((s[x] >> 3) & 0xfc) << 8) |
                             (uint32_t)((s[x] << 3) & 0xf8);
                d[x] = blend_px(d[x], p, opa);
            }
        }
        return;
    }
    for (int y = 0; y < sr.h; y++) {
        const uint32_t *s =
            (const uint32_t *)((const uint8_t *)src->pixels + (sr.y + y) * src->stride) + sr.x;
        uint16_t *d = S.fb + (dst.y + y) * S.w + dst.x;
        for (int x = 0; x < sr.w; x++) {
            uint32_t a = (s[x] >> 24) * opa / 255;
            d[x] = blend_px(d[x], s[x], a);
        }
    }
}

static void h_xform_blend(const surf_image *src, surf_rect sr, surf_rect dst_r,
                          surf_rect vis, uint8_t rot)
{
    /* nearest-neighbor inverse mapping; rot = quarter turns CCW to
     * match the P4 PPA's SRM engine. dst_r is the post-rotation
     * footprint; W0/H0 is the footprint before rotation. */
    int32_t W0 = (rot & 1) ? dst_r.h : dst_r.w;
    int32_t H0 = (rot & 1) ? dst_r.w : dst_r.h;
    if (W0 <= 0 || H0 <= 0)
        return;
    for (int y = 0; y < vis.h; y++) {
        int32_t dy = vis.y + y - dst_r.y;
        uint16_t *drow = S.fb + (vis.y + y) * S.w;
        for (int x = 0; x < vis.w; x++) {
            int32_t dx = vis.x + x - dst_r.x;
            int32_t ux, uy;
            switch (rot) {
            default: ux = dx;                    uy = dy;                    break;
            case 1:  ux = dst_r.h - 1 - dy;      uy = dx;                    break;
            case 2:  ux = dst_r.w - 1 - dx;      uy = dst_r.h - 1 - dy;      break;
            case 3:  ux = dy;                    uy = dst_r.w - 1 - dx;      break;
            }
            int32_t sx = sr.x + (int32_t)((int64_t)ux * sr.w / W0);
            int32_t sy = sr.y + (int32_t)((int64_t)uy * sr.h / H0);
            uint16_t *d = drow + vis.x + x;
            if (src->format == SURF_FMT_ARGB8888) {
                uint32_t p = *(const uint32_t *)((const uint8_t *)src->pixels +
                                                 sy * src->stride + sx * 4);
                *d = blend_px(*d, p, p >> 24);
            } else if (src->format == SURF_FMT_RGB565) {
                *d = *(const uint16_t *)((const uint8_t *)src->pixels +
                                         sy * src->stride + sx * 2);
            } else {  /* A8: alpha from image, color from tint */
                surf_color t = src->tint;
                uint32_t rgb = ((uint32_t)((t >> 8) & 0xf8) << 16) |
                               ((uint32_t)((t >> 3) & 0xfc) << 8) |
                               (uint32_t)((t << 3) & 0xf8);
                uint8_t a = *((const uint8_t *)src->pixels + sy * src->stride + sx);
                *d = blend_px(*d, rgb, a);
            }
        }
    }
}

static void h_present(const surf_rect *dirty, int n)
{
    if (S.has_scrolled) {
        /* scroll_rect moved fb pixels outside the damage system's view;
         * the dirty list only covers the exposed strip. The texture must
         * catch up over the whole shifted region — same rule as the p4
         * hal's full damage-forward on scrolled frames (DESIGN.md §5.6). */
        SDL_Rect r = {S.scrolled.x, S.scrolled.y, S.scrolled.w, S.scrolled.h};
        SDL_UpdateTexture(S.tex, &r, S.fb + r.y * S.w + r.x, S.w * 2);
        S.has_scrolled = false;
    }
    for (int i = 0; i < n; i++) {
        SDL_Rect r = {dirty[i].x, dirty[i].y, dirty[i].w, dirty[i].h};
        SDL_UpdateTexture(S.tex, &r, S.fb + r.y * S.w + r.x, S.w * 2);
    }
    SDL_RenderCopy(S.ren, S.tex, NULL, NULL);
    SDL_RenderPresent(S.ren);
}

static void h_wait_idle(void)
{
    /* Software path is synchronous; the p4 backend fences the PPA here. */
}

/* ---- services ---- */

static uint64_t h_now_us(void)
{
    return SDL_GetPerformanceCounter() * 1000000ull / SDL_GetPerformanceFrequency();
}

static bool h_poll_touch(surf_touch *out)
{
    if (S.ring_r == S.ring_w)
        return false;
    *out = S.ring[S.ring_r];
    S.ring_r = (S.ring_r + 1) % TOUCH_RING;
    return true;
}

static void *h_fb_ptr(int32_t *stride_bytes)
{
    *stride_bytes = S.w * 2;
    return S.fb;
}

static void h_scroll_rect(surf_rect r, int16_t dy)
{
    if (dy == 0)
        return;
    int16_t ady = dy < 0 ? (int16_t)-dy : dy;
    if (ady >= r.h)
        return;
    if (S.has_scrolled) {
        int16_t x1 = S.scrolled.x < r.x ? S.scrolled.x : r.x;
        int16_t y1 = S.scrolled.y < r.y ? S.scrolled.y : r.y;
        int16_t x2 = S.scrolled.x + S.scrolled.w > r.x + r.w
                         ? (int16_t)(S.scrolled.x + S.scrolled.w)
                         : (int16_t)(r.x + r.w);
        int16_t y2 = S.scrolled.y + S.scrolled.h > r.y + r.h
                         ? (int16_t)(S.scrolled.y + S.scrolled.h)
                         : (int16_t)(r.y + r.h);
        S.scrolled = (surf_rect){x1, y1, (int16_t)(x2 - x1), (int16_t)(y2 - y1)};
    } else {
        S.scrolled = r;
        S.has_scrolled = true;
    }
    size_t row_bytes = (size_t)r.w * 2;
    if (dy > 0) {  /* content up: walk top-down */
        for (int y = r.y; y < r.y + r.h - ady; y++)
            memmove(S.fb + y * S.w + r.x, S.fb + (y + ady) * S.w + r.x, row_bytes);
    } else {       /* content down: walk bottom-up */
        for (int y = r.y + r.h - 1; y >= r.y + ady; y--)
            memmove(S.fb + y * S.w + r.x, S.fb + (y - ady) * S.w + r.x, row_bytes);
    }
}

static void *h_alloc_image(size_t bytes)
{
    void *p = NULL;
    if (posix_memalign(&p, 64, (bytes + 63) & ~(size_t)63) != 0)
        return NULL;
    return p;
}

static void h_free_image(void *p)
{
    free(p);
}

static const surf_hal hal_sdl = {
    .fill = h_fill,
    .blit = h_blit,
    .blend = h_blend,
    .xform_blend = h_xform_blend,
    .present = h_present,
    .wait_idle = h_wait_idle,
    .now_us = h_now_us,
    .poll_touch = h_poll_touch,
    .alloc_image = h_alloc_image,
    .free_image = h_free_image,
    .fb_ptr = h_fb_ptr,
    .scroll_rect = h_scroll_rect,
};

/* ---- host glue ---- */

static void push_touch(int16_t x, int16_t y, uint8_t phase)
{
    int next = (S.ring_w + 1) % TOUCH_RING;
    if (next == S.ring_r)
        return;  /* full: drop; UP events still arrive next pump */
    S.ring[S.ring_w] = (surf_touch){x, y, phase};
    S.ring_w = next;
}

const surf_hal *surf_hal_sdl_init(int16_t w, int16_t h, const char *title)
{
#ifdef SURF_HAL_SDL_NO_YIELD
    /* JS drives frames (MP web build): SDL must never emscripten_sleep
     * internally — by default it sleeps in every GL SwapWindow under
     * ASYNCIFY, which aborts the synchronous VM calls. */
    SDL_SetHint(SDL_HINT_EMSCRIPTEN_ASYNCIFY, "0");
#endif
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_EVENTS) != 0)
        return NULL;
    S.w = w;
    S.h = h;
    S.win = SDL_CreateWindow(title, SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
                             w, h, SDL_WINDOW_ALLOW_HIGHDPI);
    if (!S.win)
        goto fail;
#ifdef SURF_HAL_SDL_NO_YIELD
    /* JS-driven frames (MP web build): rAF paces us, and emscripten's
     * PRESENTVSYNC path sleeps internally — an ASYNCIFY suspend the
     * synchronous VM calls can't survive. */
    S.ren = SDL_CreateRenderer(S.win, -1, SDL_RENDERER_ACCELERATED);
#else
    S.ren = SDL_CreateRenderer(S.win, -1,
                               SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
#endif
    if (!S.ren)
        S.ren = SDL_CreateRenderer(S.win, -1, 0);
    if (!S.ren)
        goto fail;
    S.tex = SDL_CreateTexture(S.ren, SDL_PIXELFORMAT_RGB565,
                              SDL_TEXTUREACCESS_STREAMING, w, h);
    S.fb = h_alloc_image((size_t)w * h * 2);
    if (!S.tex || !S.fb)
        goto fail;
    memset(S.fb, 0, (size_t)w * h * 2);
    SDL_StartTextInput();
    return &hal_sdl;
fail:
    surf_hal_sdl_quit();
    return NULL;
}

void surf_hal_sdl_quit(void)
{
    if (S.fb) h_free_image(S.fb);
    if (S.tex) SDL_DestroyTexture(S.tex);
    if (S.ren) SDL_DestroyRenderer(S.ren);
    if (S.win) SDL_DestroyWindow(S.win);
    memset(&S, 0, sizeof S);
    SDL_Quit();
}

bool surf_hal_sdl_dump_screen_ppm(const char *path)
{
    /* What the user actually sees: the streaming texture, not S.fb.
     * The two only match if present kept the texture coherent — this
     * exists to catch paths (like scroll_rect) that move fb pixels
     * outside the damage system's view. */
    if (!S.ren || !S.tex)
        return false;
    int ow, oh;
    if (SDL_GetRendererOutputSize(S.ren, &ow, &oh) != 0 || ow < S.w || oh < S.h)
        return false;
    uint16_t *px = malloc((size_t)ow * oh * 2);
    if (!px)
        return false;
    SDL_RenderCopy(S.ren, S.tex, NULL, NULL);
    if (SDL_RenderReadPixels(S.ren, NULL, SDL_PIXELFORMAT_RGB565, px,
                             ow * 2) != 0) {
        free(px);
        return false;
    }
    FILE *f = fopen(path, "wb");
    if (!f) {
        free(px);
        return false;
    }
    fprintf(f, "P6\n%d %d\n255\n", S.w, S.h);
    for (int y = 0; y < S.h; y++) {
        for (int x = 0; x < S.w; x++) {
            /* nearest sample handles the hidpi output scale */
            uint16_t p = px[(int64_t)(y * oh / S.h) * ow + x * ow / S.w];
            uint8_t rgb[3] = {
                (uint8_t)(((p >> 8) & 0xf8) | (p >> 13)),
                (uint8_t)(((p >> 3) & 0xfc) | ((p >> 9) & 0x03)),
                (uint8_t)(((p << 3) & 0xf8) | ((p >> 2) & 0x07)),
            };
            fwrite(rgb, 1, 3, f);
        }
    }
    fclose(f);
    free(px);
    return true;
}

bool surf_hal_sdl_dump_ppm(const char *path)
{
    FILE *f = fopen(path, "wb");
    if (!f || !S.fb)
        return false;
    fprintf(f, "P6\n%d %d\n255\n", S.w, S.h);
    for (int i = 0; i < S.w * S.h; i++) {
        uint16_t p = S.fb[i];
        uint8_t rgb[3] = {
            (uint8_t)(((p >> 8) & 0xf8) | (p >> 13)),
            (uint8_t)(((p >> 3) & 0xfc) | ((p >> 9) & 0x03)),
            (uint8_t)(((p << 3) & 0xf8) | ((p >> 2) & 0x07)),
        };
        fwrite(rgb, 1, 3, f);
    }
    fclose(f);
    return true;
}

bool surf_hal_sdl_pump(void)
{
#if defined(__EMSCRIPTEN__) && !defined(SURF_HAL_SDL_NO_YIELD)
    /* ASYNCIFY yield: suspend until the next animation frame — this is
     * how the desktop `while (pump()) tick;` shape runs unchanged in a
     * canvas, and it must be requestAnimationFrame, not a timer: frames
     * drawn from timer-resumed contexts are not reliably composited
     * (observed in Chrome), and rAF paces to vsync for free. Yield
     * lives here, not in present: present is skipped entirely on
     * damage-free frames. The MicroPython web build defines
     * SURF_HAL_SDL_NO_YIELD instead: an ASYNCIFY suspend inside MP's
     * import machinery wedges the VM, so there the browser drives
     * frames from JS and every call into the VM stays synchronous. */
    surf_web_raf_yield();
#endif
    SDL_Event e;
    while (SDL_PollEvent(&e)) {
        switch (e.type) {
        case SDL_QUIT:
            return false;
        case SDL_KEYDOWN: {
            bool shift = (e.key.keysym.mod & KMOD_SHIFT) != 0;
            switch (e.key.keysym.sym) {
            case SDLK_ESCAPE:    return false;
            case SDLK_LEFT:      push_key(SURF_KEY_LEFT, shift, NULL); break;
            case SDLK_RIGHT:     push_key(SURF_KEY_RIGHT, shift, NULL); break;
            case SDLK_UP:        push_key(SURF_KEY_UP, shift, NULL); break;
            case SDLK_DOWN:      push_key(SURF_KEY_DOWN, shift, NULL); break;
            case SDLK_PAGEUP:    push_key(SURF_KEY_PGUP, shift, NULL); break;
            case SDLK_PAGEDOWN:  push_key(SURF_KEY_PGDN, shift, NULL); break;
            case SDLK_HOME:      push_key(SURF_KEY_HOME, shift, NULL); break;
            case SDLK_END:       push_key(SURF_KEY_END, shift, NULL); break;
            case SDLK_BACKSPACE: push_key(SURF_KEY_BACKSPACE, shift, NULL); break;
            case SDLK_DELETE:    push_key(SURF_KEY_DELETE, shift, NULL); break;
            case SDLK_RETURN:    push_key(SURF_KEY_ENTER, shift, NULL); break;
            }
            break;
        }
        case SDL_TEXTINPUT:
            push_key(SURF_KEY_TEXT, false, e.text.text);
            break;
        case SDL_MOUSEBUTTONDOWN:
            if (e.button.button == SDL_BUTTON_LEFT) {
                S.mouse_down = true;
                push_touch((int16_t)e.button.x, (int16_t)e.button.y, SURF_TOUCH_DOWN);
            }
            break;
        case SDL_MOUSEMOTION:
            if (S.mouse_down)
                push_touch((int16_t)e.motion.x, (int16_t)e.motion.y, SURF_TOUCH_MOVE);
            break;
        case SDL_MOUSEBUTTONUP:
            if (e.button.button == SDL_BUTTON_LEFT) {
                S.mouse_down = false;
                push_touch((int16_t)e.button.x, (int16_t)e.button.y, SURF_TOUCH_UP);
            }
            break;
        }
    }
    return true;
}
