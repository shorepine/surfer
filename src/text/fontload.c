/* Runtime font loading: parse a fontbake blob (the "SFN1" format it
 * emits to a .py bytes module) into a surf_font. Same idea as
 * surf_image_from_png — decode once at load time, never in the frame
 * path. Lets a program pick a textgrid font at runtime instead of the
 * one baked into the firmware. */
#include <stdlib.h>
#include <string.h>

#include "surf_internal.h"

static uint32_t rd_u16(const uint8_t *p) { return p[0] | (p[1] << 8); }
static uint32_t rd_u32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}
static int16_t rd_i16(const uint8_t *p) { return (int16_t)rd_u16(p); }

surf_font *surf_font_from_blob(const void *data, size_t len)
{
    const uint8_t *b = data;
    if (!surf_g.hal || len < 24 || memcmp(b, "SFN1", 4) != 0)
        return NULL;
    int16_t aw = (int16_t)rd_u16(b + 4), ah = (int16_t)rd_u16(b + 6);
    int16_t ascent = rd_i16(b + 8), descent = rd_i16(b + 10), gap = rd_i16(b + 12);
    int32_t ng = (int32_t)rd_u32(b + 16), nk = (int32_t)rd_u32(b + 20);
    if (aw <= 0 || ah <= 0 || ng < 0 || nk < 0)
        return NULL;
    size_t need = 24 + (size_t)aw * ah + (size_t)ng * 18 + (size_t)nk * 10;
    if (len < need)
        return NULL;

    surf_font *f = calloc(1, sizeof *f);
    surf_glyph *gl = ng ? calloc((size_t)ng, sizeof *gl) : NULL;
    surf_kern *kn = nk ? calloc((size_t)nk, sizeof *kn) : NULL;
    /* atlas width is a power of two from packing, so stride == w is
     * already 64-byte aligned for the PPA */
    uint8_t *px = surf_g.hal->alloc_image((size_t)aw * ah);
    if (!f || (ng && !gl) || (nk && !kn) || !px) {
        if (px) surf_g.hal->free_image(px);
        free(f); free(gl); free(kn);
        return NULL;
    }

    const uint8_t *ap = b + 24;
    memcpy(px, ap, (size_t)aw * ah);
    const uint8_t *gp = ap + (size_t)aw * ah;
    for (int32_t i = 0; i < ng; i++) {
        const uint8_t *g = gp + (size_t)i * 18;
        gl[i] = (surf_glyph){
            .cp = rd_u32(g), .x = rd_i16(g + 4), .y = rd_i16(g + 6),
            .w = rd_i16(g + 8), .h = rd_i16(g + 10),
            .xoff = rd_i16(g + 12), .yoff = rd_i16(g + 14), .adv = rd_i16(g + 16),
        };
    }
    const uint8_t *kp = gp + (size_t)ng * 18;
    for (int32_t i = 0; i < nk; i++) {
        const uint8_t *k = kp + (size_t)i * 10;
        kn[i] = (surf_kern){.a = rd_u32(k), .b = rd_u32(k + 4), .adv = rd_i16(k + 8)};
    }

    *f = (surf_font){
        .atlas = {.pixels = px, .w = aw, .h = ah, .stride = aw,
                  .format = SURF_FMT_A8, .opaque = false},
        .ascent = ascent, .descent = descent, .line_gap = gap,
        .glyphs = gl, .nglyphs = ng, .kerns = kn, .nkerns = nk,
    };
    return f;
}

void surf_font_free(surf_font *f)
{
    if (!f)
        return;
    if (f->atlas.pixels && surf_g.hal)
        surf_g.hal->free_image(f->atlas.pixels);
    free((void *)f->glyphs);
    free((void *)f->kerns);
    free(f);
}
