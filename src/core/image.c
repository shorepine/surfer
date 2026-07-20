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

void surf_image_destroy(surf_image *img)
{
    if (!img)
        return;
    if (img->pixels && surf_g.hal)
        surf_g.hal->free_image(img->pixels);
    free(img);
}
