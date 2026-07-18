/* SDL2 backend: RGB565 software framebuffer + streaming texture. This file
 * is the only place outside build tools where per-pixel loops are allowed;
 * on device the same ops are PPA/2D-DMA jobs. */
#include <SDL.h>
#include <stdlib.h>
#include <string.h>

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

static void h_scale_blit(const surf_image *src, surf_rect src_r, surf_rect dst_r)
{
    /* Exists in the vtable per DESIGN.md §5.4; nothing uses it in v1. */
    (void)src; (void)src_r; (void)dst_r;
}

static void h_present(const surf_rect *dirty, int n)
{
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
    .scale_blit = h_scale_blit,
    .present = h_present,
    .wait_idle = h_wait_idle,
    .now_us = h_now_us,
    .poll_touch = h_poll_touch,
    .alloc_image = h_alloc_image,
    .free_image = h_free_image,
    .fb_ptr = h_fb_ptr,
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
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_EVENTS) != 0)
        return NULL;
    S.w = w;
    S.h = h;
    S.win = SDL_CreateWindow(title, SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
                             w, h, SDL_WINDOW_ALLOW_HIGHDPI);
    if (!S.win)
        goto fail;
    S.ren = SDL_CreateRenderer(S.win, -1,
                               SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
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
            case SDLK_RETURN:    break;
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
