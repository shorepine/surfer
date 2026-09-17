/* Built-in widget art baked at RUNTIME, on first use, into an image the
 * hal allocated — instead of shipping it as flash .rodata.
 *
 * WHY: the three knob filmstrips (64 frames each of a 64, 40 and 56 px
 * face) were 565 KB of a device image that had 192 bytes of partition
 * left, and on the P4 they were being memcpy'd out of flash into PSRAM at
 * init anyway (the PPA cannot DMA from memory-mapped flash), so a strip
 * baked straight into that PSRAM costs the flash nothing and the frame
 * path nothing: the knob still picks a pre-rendered frame and the blend
 * is the same A8 op over the same pixels. What moved is 64 faces' worth
 * of arithmetic from a laptop at bake time to the board at first use.
 *
 * THE FACES ARE tools/gen_widget_assets.py's, PORTED LINE FOR LINE —
 * knob_strip() and selector_strip() there are the reference and stay in
 * that file for exactly that reason; a change to the look is made THERE
 * first and mirrored here, and the two are compared pixel for pixel by
 * the check that landed this. The arithmetic is float where the Python
 * was double: the P4 has a single-precision FPU and a double is soft, so
 * an ink value that sits exactly on a truncation boundary may come out a
 * unit different. Invisible, and checked to be no worse than that.
 *
 * LAZY, and never freed: a knob is a thing most sessions make and some
 * never do (a game), so the bake lands on the first surf_knob_new of a
 * size rather than in surf_init — and once made a strip is shared by
 * every widget of that size for the life of the process, appicon's rule,
 * since a widget copies the surf_image struct and points at these
 * pixels. A soft reset does not free it either: surf_deinit frees the
 * node pool and nothing an image owns, so the cache survives and the
 * second session pays nothing. */
#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "surfer.h"

#define ART_FRAMES SURF_ART_FRAMES
#define ART_KINDS  2
#define ART_CACHE  6       /* (kind, size) pairs remembered */

enum { ART_KNOB = 0, ART_SELECTOR = 1 };

static struct {
    int16_t     kind, size;
    surf_image *img;
} cache[ART_CACHE];

static int clampi(float v)
{
    int i = (int)v;          /* Python's int(): truncation toward zero */
    return i < 0 ? 0 : i > 255 ? 255 : i;
}

/* A colour and a coverage as ONE A8 byte: Rec.601 luma of the colour,
 * scaled by the coverage. See ink() in the generator for the trade. */
static uint8_t ink(int r, int g, int b, float a)
{
    int lum = (r * 77 + g * 150 + b * 29) >> 8;
    return (uint8_t)clampi(a * (float)lum);
}

static float seg_dist(float px, float py, float ax, float ay, float bx, float by)
{
    float vx = bx - ax, vy = by - ay;
    float t = ((px - ax) * vx + (py - ay) * vy) / (vx * vx + vy * vy);
    if (t < 0.0f) t = 0.0f;
    if (t > 1.0f) t = 1.0f;
    return hypotf(px - (ax + vx * t), py - (ay + vy * t));
}

static float clamp01(float v)
{
    return v < 0.0f ? 0.0f : v > 1.0f ? 1.0f : v;
}

/* knob_strip(): a dark-rimmed grey knob, a specular highlight at the top
 * left, a bright hairline pointer sweeping -135..+135 over the frames.
 *
 * THE BODY IS THE SAME IN EVERY FRAME, so its ink is computed once per
 * pixel and every frame starts as a copy of it; the sweep only touches
 * the pixels inside the pointer's own bounding box, where the mix with
 * the body colour is redone from the body's colour and coverage. That is
 * the difference between 64 full faces and one plus 64 small patches —
 * measured on the P4X, 155 ms -> 30 ms for the 64 px strip, same bytes. */
typedef struct { uint8_t r, g, b, ink; float a; } body_px;

static void bake_knob(surf_image *img, int size)
{
    const float c = size / 2.0f;
    const float body_r = c - 5.0f, edge_r = c - 0.5f;
    float ptr_w = size / 24.0f;
    if (ptr_w < 1.8f) ptr_w = 1.8f;
    body_px *body = malloc((size_t)size * size * sizeof *body);
    if (!body)
        return;
    for (int y = 0; y < size; y++)
        for (int x = 0; x < size; x++) {
            body_px *o = &body[y * size + x];
            float fx = x + 0.5f, fy = y + 0.5f;
            float r = hypotf(fx - c, fy - c);
            o->a = clamp01((edge_r - r) * 2.0f);
            if (o->a == 0.0f) {
                o->ink = 0;
                continue;
            }
            if (r > body_r) {
                o->r = 58; o->g = 62; o->b = 74;
            } else {
                float sh = 1.0f - 0.45f * (r / body_r) * (r / body_r);
                float hr = hypotf((fx - c) / body_r + 0.35f,
                                  (fy - c) / body_r + 0.35f);
                float s = 1.0f - hr * 1.8f;
                if (s < 0.0f) s = 0.0f;
                float spec = s * s * s * 140.0f;
                o->r = (uint8_t)clampi(126.0f * sh + spec);
                o->g = (uint8_t)clampi(130.0f * sh + spec);
                o->b = (uint8_t)clampi(142.0f * sh + spec);
            }
            o->ink = ink(o->r, o->g, o->b, o->a);
        }
    uint8_t *px = img->pixels;
    for (int f = 0; f < ART_FRAMES; f++) {
        float ang = (-135.0f + 270.0f * f / (ART_FRAMES - 1)) * (float)M_PI / 180.0f;
        float dx = sinf(ang), dy = -cosf(ang);
        float ax = c + dx * size * 0.125f, ay = c + dy * size * 0.125f;
        float bx = c + dx * size * 0.345f, by = c + dy * size * 0.345f;
        /* the pixels the pointer can touch: its segment, plus the reach
         * of the mix (ptr_w + 0.5) and a pixel of slack either way */
        float reach = ptr_w + 1.5f;
        int x0 = (int)floorf((ax < bx ? ax : bx) - reach), x1 = (int)ceilf((ax > bx ? ax : bx) + reach);
        int y0 = (int)floorf((ay < by ? ay : by) - reach), y1 = (int)ceilf((ay > by ? ay : by) + reach);
        if (x0 < 0) x0 = 0;
        if (y0 < 0) y0 = 0;
        if (x1 > size - 1) x1 = size - 1;
        if (y1 > size - 1) y1 = size - 1;
        for (int y = 0; y < size; y++) {
            uint8_t *row = px + (size_t)y * img->stride + (size_t)f * size;
            const body_px *o = &body[y * size];
            for (int x = 0; x < size; x++)
                row[x] = o[x].ink;
            if (y < y0 || y > y1)
                continue;
            for (int x = x0; x <= x1; x++) {
                if (o[x].a == 0.0f)
                    continue;
                float d = seg_dist(x + 0.5f, y + 0.5f, ax, ay, bx, by);
                float t = clamp01(ptr_w + 0.5f - d);
                if (t == 0.0f)
                    continue;
                int cr = clampi(o[x].r + (240 - o[x].r) * t);
                int cg = clampi(o[x].g + (242 - o[x].g) * t);
                int cb = clampi(o[x].b + (248 - o[x].b) * t);
                row[x] = ink(cr, cg, cb, o[x].a);
            }
        }
    }
    free(body);
}

/* selector_strip(): a light chrome knob with a hard-edged bright wedge
 * and a dark spindle, for the detented selector on a pale panel.
 *
 * Same split: the body — shading, rim, spindle — is one pass, and the
 * wedge is the only thing that turns. Where the wedge can land (inside
 * the rim's margin, outside the spindle) the coverage is full, so its
 * ink is one constant; per frame a candidate pixel costs the rotation
 * and two compares. */
static void bake_selector(surf_image *img, int size)
{
    const float c = size / 2.0f;
    const float body_r = c - 2.0f;
    uint8_t *body = malloc((size_t)size * size);
    uint8_t *cand = malloc((size_t)size * size);   /* 1 where a wedge may land */
    if (!body || !cand) {
        free(body);
        free(cand);
        return;
    }
    const uint8_t wedge_ink = ink(252, 252, 254, 1.0f);
    for (int y = 0; y < size; y++)
        for (int x = 0; x < size; x++) {
            int i = y * size + x;
            float dx = x + 0.5f - c, dy = y + 0.5f - c;
            float r = hypotf(dx, dy);
            float a = clamp01((body_r - r) * 1.6f + 0.5f);
            cand[i] = 0;
            if (a == 0.0f) {
                body[i] = 0;
                continue;
            }
            float sh = 0.72f + 0.42f * (-(dx + dy) / (2.0f * body_r) + 0.5f);
            int cr = clampi(196.0f * sh), cg = clampi(198.0f * sh),
                cb = clampi(206.0f * sh);
            if (r > body_r - 2.0f) {
                cr = clampi(cr * 0.55f);
                cg = clampi(cg * 0.55f);
                cb = clampi(cb * 0.55f);
            }
            if (r < body_r * 0.17f) {         /* the spindle beats the wedge */
                cr = clampi(196.0f * 0.42f);
                cg = clampi(198.0f * 0.42f);
                cb = clampi(206.0f * 0.42f);
            } else if (r < body_r - 2.5f) {
                cand[i] = 1;
            }
            body[i] = ink(cr, cg, cb, a);
        }
    uint8_t *px = img->pixels;
    for (int f = 0; f < ART_FRAMES; f++) {
        float ang = (-135.0f + 270.0f * f / (ART_FRAMES - 1)) * (float)M_PI / 180.0f;
        float ca = cosf(-ang), sa = sinf(-ang);
        for (int y = 0; y < size; y++) {
            uint8_t *row = px + (size_t)y * img->stride + (size_t)f * size;
            memcpy(row, body + y * size, (size_t)size);
            const uint8_t *cd = cand + y * size;
            float dy = y + 0.5f - c;
            for (int x = 0; x < size; x++) {
                if (!cd[x])
                    continue;
                float dx = x + 0.5f - c;
                float wx = dx * ca - dy * sa, wy = dx * sa + dy * ca;
                if (wy < 0.0f && fabsf(wx) < (-wy) * 0.24f + 1.0f)
                    row[x] = wedge_ink;
            }
        }
    }
    free(body);
    free(cand);
}

static const surf_image *art_strip(int kind, int16_t size)
{
    if (size < 8 || size > 256)
        return NULL;
    int free_slot = -1;
    for (int i = 0; i < ART_CACHE; i++) {
        if (cache[i].img && cache[i].kind == kind && cache[i].size == size)
            return cache[i].img;
        if (!cache[i].img && free_slot < 0)
            free_slot = i;
    }
    if (free_slot < 0)
        return NULL;   /* six distinct strips is more than any panel has */
    surf_image *img = surf_image_new((int16_t)(size * ART_FRAMES), size,
                                     SURF_FMT_A8);
    if (!img)
        return NULL;
    if (kind == ART_KNOB)
        bake_knob(img, size);
    else
        bake_selector(img, size);
    surf_image_flush(img);   /* the PPA reads memory, not the cache */
    cache[free_slot].kind = (int16_t)kind;
    cache[free_slot].size = size;
    cache[free_slot].img = img;
    return img;
}

const surf_image *surf_art_knob_strip(int16_t size)
{
    return art_strip(ART_KNOB, size);
}

const surf_image *surf_art_selector_strip(int16_t size)
{
    return art_strip(ART_SELECTOR, size);
}
