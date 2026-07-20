/* Runtime images: PNG → ARGB8888 surf_image, pixels from hal->alloc_image.
 * Decode happens at load time, never in the frame path — a sprite is
 * still a pre-rendered asset, it just arrived after boot. */
#include <stdlib.h>
#include <string.h>

#include "surf_internal.h"

#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_PNG
#define STBI_NO_STDIO
#define STBI_NO_HDR
#define STBI_NO_LINEAR
#define STBI_NO_SIMD
#define STBI_NO_FAILURE_STRINGS
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-function"
#include "stb/stb_image.h"
#pragma GCC diagnostic pop

surf_image *surf_image_from_png(const void *data, size_t len)
{
    if (!surf_g.hal)
        return NULL;
    int w, h, comp;
    unsigned char *rgba = stbi_load_from_memory(data, (int)len, &w, &h, &comp, 4);
    if (!rgba)
        return NULL;

    surf_image *img = malloc(sizeof *img);
    if (!img) {
        stbi_image_free(rgba);
        return NULL;
    }
    int32_t stride = ((int32_t)w * 4 + 63) & ~63;  /* device: 64B rows */
    uint8_t *px = surf_g.hal->alloc_image((size_t)stride * h);
    if (!px) {
        stbi_image_free(rgba);
        free(img);
        return NULL;
    }

    bool opaque = true;
    for (int y = 0; y < h; y++) {
        const unsigned char *s = rgba + (size_t)y * w * 4;
        uint32_t *d = (uint32_t *)(px + (size_t)y * stride);
        for (int x = 0; x < w; x++) {
            uint8_t r = s[x * 4], g = s[x * 4 + 1], b = s[x * 4 + 2], a = s[x * 4 + 3];
            if (a != 255)
                opaque = false;
            d[x] = ((uint32_t)a << 24) | ((uint32_t)r << 16) |
                   ((uint32_t)g << 8) | b;
        }
    }
    stbi_image_free(rgba);

    *img = (surf_image){
        .pixels = px,
        .w = (int16_t)w,
        .h = (int16_t)h,
        .stride = stride,
        .format = SURF_FMT_ARGB8888,
        .opaque = opaque,
    };
    return img;
}

surf_image *surf_image_from_png_a8(const void *data, size_t len)
{
    if (!surf_g.hal)
        return NULL;
    int w, h, comp;
    unsigned char *rgba = stbi_load_from_memory(data, (int)len, &w, &h, &comp, 4);
    if (!rgba)
        return NULL;
    surf_image *img = malloc(sizeof *img);
    int32_t stride = ((int32_t)w + 63) & ~63;
    uint8_t *px = img ? surf_g.hal->alloc_image((size_t)stride * h) : NULL;
    if (!px) {
        stbi_image_free(rgba);
        free(img);
        return NULL;
    }
    for (int y = 0; y < h; y++) {
        const unsigned char *s = rgba + (size_t)y * w * 4;
        uint8_t *d = px + (size_t)y * stride;
        for (int x = 0; x < w; x++)
            d[x] = s[x * 4 + 3];
    }
    stbi_image_free(rgba);
    *img = (surf_image){
        .pixels = px,
        .w = (int16_t)w,
        .h = (int16_t)h,
        .stride = stride,
        .format = SURF_FMT_A8,
        .opaque = false,
        .tint = 0xffff,
    };
    return img;
}

void surf_image_destroy(surf_image *img)
{
    if (!img)
        return;
    if (img->pixels && surf_g.hal)
        surf_g.hal->free_image(img->pixels);
    free(img);
}

/* ---- load-time composition (never per frame) ---- */

surf_image *surf_image_new(int16_t w, int16_t h, surf_format format)
{
    if (!surf_g.hal || w <= 0 || h <= 0 || format > SURF_FMT_A8)
        return NULL;
    int bpp = format == SURF_FMT_ARGB8888 ? 4 : format == SURF_FMT_A8 ? 1 : 2;
    int32_t stride = ((int32_t)w * bpp + 63) & ~63;
    uint8_t *px = surf_g.hal->alloc_image((size_t)stride * h);
    surf_image *img = malloc(sizeof *img);
    if (!px || !img) {
        if (px) surf_g.hal->free_image(px);
        free(img);
        return NULL;
    }
    memset(px, 0, (size_t)stride * h);  /* 565: black; ARGB: transparent */
    *img = (surf_image){
        .pixels = px, .w = w, .h = h, .stride = stride,
        .format = (uint8_t)format,
        .opaque = format == SURF_FMT_RGB565,
        .tint = 0xffff,   /* A8 masks start white */
    };
    return img;
}

void surf_image_fill(surf_image *dst, surf_rect r, surf_color c)
{
    if (!dst || !dst->pixels)
        return;
    r = surf_rect_intersect(r, (surf_rect){0, 0, dst->w, dst->h});
    if (surf_rect_empty(r))
        return;
    if (dst->format == SURF_FMT_RGB565) {
        for (int y = 0; y < r.h; y++) {
            uint16_t *row = (uint16_t *)((uint8_t *)dst->pixels +
                                         (r.y + y) * dst->stride) + r.x;
            for (int x = 0; x < r.w; x++)
                row[x] = c;
        }
    } else if (dst->format == SURF_FMT_ARGB8888) {
        uint32_t p = 0xff000000u |
                     ((uint32_t)((c >> 8) & 0xf8) << 16) |
                     ((uint32_t)((c >> 3) & 0xfc) << 8) |
                     (uint32_t)((c << 3) & 0xf8);
        for (int y = 0; y < r.h; y++) {
            uint32_t *row = (uint32_t *)((uint8_t *)dst->pixels +
                                         (r.y + y) * dst->stride) + r.x;
            for (int x = 0; x < r.w; x++)
                row[x] = p;
        }
    }
}

/* read any supported source pixel as (a, 0xRRGGBB) */
static uint32_t src_px(const surf_image *img, int x, int y, uint32_t *a)
{
    switch (img->format) {
    case SURF_FMT_ARGB8888: {
        uint32_t p = *(const uint32_t *)((const uint8_t *)img->pixels +
                                         y * img->stride + x * 4);
        *a = p >> 24;
        return p & 0xffffff;
    }
    case SURF_FMT_A8: {
        *a = *((const uint8_t *)img->pixels + y * img->stride + x);
        surf_color t = img->tint;
        return ((uint32_t)((t >> 8) & 0xf8) << 16) |
               ((uint32_t)((t >> 3) & 0xfc) << 8) |
               (uint32_t)((t << 3) & 0xf8);
    }
    default: {
        uint16_t p = *(const uint16_t *)((const uint8_t *)img->pixels +
                                         y * img->stride + x * 2);
        *a = 255;
        return ((uint32_t)((p >> 8) & 0xf8) << 16) |
               ((uint32_t)((p >> 3) & 0xfc) << 8) |
               (uint32_t)((p << 3) & 0xf8);
    }
    }
}

void surf_image_blit_rot(surf_image *dst, const surf_image *src,
                         surf_rect sr, int16_t x, int16_t y, uint8_t rot)
{
    rot &= 3;
    if (rot == 0) {
        surf_image_blit(dst, src, sr, x, y);
        return;
    }
    if (!dst || !src || !dst->pixels || !src->pixels)
        return;
    sr = surf_rect_intersect(sr, (surf_rect){0, 0, src->w, src->h});
    int16_t ow = (rot & 1) ? sr.h : sr.w;   /* rotated footprint */
    int16_t oh = (rot & 1) ? sr.w : sr.h;
    for (int j = 0; j < oh; j++) {
        int16_t dyp = (int16_t)(y + j);
        if (dyp < 0 || dyp >= dst->h)
            continue;
        for (int i = 0; i < ow; i++) {
            int16_t dxp = (int16_t)(x + i);
            if (dxp < 0 || dxp >= dst->w)
                continue;
            int32_t ux, uy;   /* same CCW mapping as the hal xform */
            switch (rot) {
            case 1:  ux = oh - 1 - j; uy = i;            break;
            case 2:  ux = ow - 1 - i; uy = oh - 1 - j;   break;
            default: ux = j;          uy = ow - 1 - i;   break;
            }
            uint32_t a, rgb = src_px(src, sr.x + ux, sr.y + uy, &a);
            if (a == 0)
                continue;
            uint32_t argb = (a << 24) | rgb;
            surf_image one = {.pixels = &argb, .w = 1, .h = 1, .stride = 4,
                              .format = SURF_FMT_ARGB8888};
            surf_image_blit(dst, &one, (surf_rect){0, 0, 1, 1}, dxp, dyp);
        }
    }
}

void surf_image_blit(surf_image *dst, const surf_image *src, surf_rect sr,
                     int16_t x, int16_t y)
{
    if (!dst || !src || !dst->pixels || !src->pixels)
        return;
    sr = surf_rect_intersect(sr, (surf_rect){0, 0, src->w, src->h});
    /* clip the destination, dragging the source window along */
    if (x < 0) { sr.x -= x; sr.w += x; x = 0; }
    if (y < 0) { sr.y -= y; sr.h += y; y = 0; }
    if (x + sr.w > dst->w) sr.w = dst->w - x;
    if (y + sr.h > dst->h) sr.h = dst->h - y;
    if (sr.w <= 0 || sr.h <= 0)
        return;

    for (int j = 0; j < sr.h; j++) {
        for (int i = 0; i < sr.w; i++) {
            uint32_t a, rgb = src_px(src, sr.x + i, sr.y + j, &a);
            if (a == 0)
                continue;
            uint32_t r = (rgb >> 16) & 0xff, g = (rgb >> 8) & 0xff, b = rgb & 0xff;
            if (dst->format == SURF_FMT_RGB565) {
                uint16_t *d = (uint16_t *)((uint8_t *)dst->pixels +
                                           (y + j) * dst->stride) + x + i;
                uint32_t dr = (uint32_t)((*d >> 8) & 0xf8) | (*d >> 13);
                uint32_t dg = (uint32_t)((*d >> 3) & 0xfc) | ((*d >> 9) & 0x03);
                uint32_t db = (uint32_t)((*d << 3) & 0xf8) | ((*d >> 2) & 0x07);
                uint32_t nr = (r * a + dr * (255 - a) + 127) / 255;
                uint32_t ng = (g * a + dg * (255 - a) + 127) / 255;
                uint32_t nb = (b * a + db * (255 - a) + 127) / 255;
                *d = (uint16_t)(((nr & 0xf8) << 8) | ((ng & 0xfc) << 3) | (nb >> 3));
            } else if (dst->format == SURF_FMT_A8) {  /* coverage-over */
                uint8_t *d = (uint8_t *)dst->pixels + (y + j) * dst->stride + x + i;
                *d = (uint8_t)(a + *d * (255 - a) / 255);
            } else {  /* ARGB dst: src-over */
                uint32_t *d = (uint32_t *)((uint8_t *)dst->pixels +
                                           (y + j) * dst->stride) + x + i;
                uint32_t da = *d >> 24;
                uint32_t dr = (*d >> 16) & 0xff, dg = (*d >> 8) & 0xff, db = *d & 0xff;
                uint32_t oa = a + da * (255 - a) / 255;
                uint32_t orr = (r * a + dr * da * (255 - a) / 255) / (oa ? oa : 1);
                uint32_t og = (g * a + dg * da * (255 - a) / 255) / (oa ? oa : 1);
                uint32_t ob = (b * a + db * da * (255 - a) / 255) / (oa ? oa : 1);
                *d = (oa << 24) | (orr << 16) | (og << 8) | ob;
            }
        }
    }
}
