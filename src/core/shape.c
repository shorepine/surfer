/* Load-time vector shapes baked into surf_images. This is the tool that
 * makes "a better asset" — the frame path never sees a bezier, only the
 * finished pixels (sprite/layer material like any PNG). Never call any
 * of this per frame.
 *
 * Engine: one scanline coverage rasterizer. Everything — thick lines,
 * ellipses, bezier strokes — flattens to edges first; fills use nonzero
 * winding (so overlapping stroke quads and round joins union cleanly).
 * AA is 4x vertical supersampling with exact fractional span coverage
 * horizontally. Float math and malloc are fine here for the same reason
 * they are in image.c: this is bake-time code, not the frame path. */
#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "surf_internal.h"

typedef struct { float x0, y0, x1, y1; } sh_edge;

typedef struct {
    sh_edge *e;
    int      n, cap;
    float    miny, maxy;
} sh_path;

static bool path_edge(sh_path *p, float x0, float y0, float x1, float y1)
{
    if (y0 == y1)
        return true;   /* horizontal edges never cross a scanline */
    if (p->n == p->cap) {
        int cap = p->cap ? p->cap * 2 : 64;
        sh_edge *e = realloc(p->e, (size_t)cap * sizeof *e);
        if (!e)
            return false;
        p->e = e;
        p->cap = cap;
    }
    p->e[p->n++] = (sh_edge){x0, y0, x1, y1};
    if (y0 < p->miny) p->miny = y0;
    if (y1 < p->miny) p->miny = y1;
    if (y0 > p->maxy) p->maxy = y0;
    if (y1 > p->maxy) p->maxy = y1;
    return true;
}

/* All subpaths are normalized to one orientation before their edges go
 * in: with nonzero winding that makes every overlap ADDITIVE (union),
 * so stroke quads and their join discs merge instead of cancelling
 * where they cross. */
static bool path_poly(sh_path *p, const float *xy, int n)
{
    float area2 = 0.0f;
    for (int i = 0; i < n; i++) {
        int j = (i + 1) % n;
        area2 += xy[i * 2] * xy[j * 2 + 1] - xy[j * 2] * xy[i * 2 + 1];
    }
    for (int i = 0; i < n; i++) {
        int j = (i + 1) % n;
        bool ok = area2 >= 0.0f
            ? path_edge(p, xy[i * 2], xy[i * 2 + 1], xy[j * 2], xy[j * 2 + 1])
            : path_edge(p, xy[j * 2], xy[j * 2 + 1], xy[i * 2], xy[i * 2 + 1]);
        if (!ok)
            return false;
    }
    return true;
}

/* a full circle as a polygon subpath (round caps and joins) */
static bool path_circle(sh_path *p, float cx, float cy, float r)
{
    int segs = (int)(r * 0.9f);
    if (segs < 8) segs = 8;
    if (segs > 64) segs = 64;
    float xy[64 * 2];
    for (int i = 0; i < segs; i++) {
        float a = (float)i * 2.0f * (float)M_PI / segs;
        xy[i * 2] = cx + r * cosf(a);
        xy[i * 2 + 1] = cy + r * sinf(a);
    }
    return path_poly(p, xy, segs);
}

/* stroke a polyline: one quad per segment + a disc at every vertex.
 * Nonzero winding turns the pile of overlaps into a clean union. */
static bool path_stroke(sh_path *p, const float *xy, int n, float hw)
{
    for (int i = 0; i < n - 1; i++) {
        float x0 = xy[i * 2], y0 = xy[i * 2 + 1];
        float x1 = xy[i * 2 + 2], y1 = xy[i * 2 + 3];
        float dx = x1 - x0, dy = y1 - y0;
        float len = sqrtf(dx * dx + dy * dy);
        if (len < 1e-6f)
            continue;
        float nx = -dy / len * hw, ny = dx / len * hw;
        float quad[8] = {x0 + nx, y0 + ny, x1 + nx, y1 + ny,
                         x1 - nx, y1 - ny, x0 - nx, y0 - ny};
        if (!path_poly(p, quad, 4))
            return false;
    }
    for (int i = 0; i < n; i++)
        if (!path_circle(p, xy[i * 2], xy[i * 2 + 1], hw))
            return false;
    return true;
}

/* ---- paint ---- */

typedef struct {
    /* endpoint colors as 8-bit channels */
    int r0, g0, b0, a0, r1, g1, b1, a1;
    float gx, gy, gdx, gdy, inv_len2;  /* gradient axis; inv_len2 = 0 → solid */
} sh_paint;

static sh_paint paint_prep(const surf_paint *p)
{
    sh_paint q;
    q.r0 = (p->c0 >> 8) & 0xf8; q.r0 |= q.r0 >> 5;
    q.g0 = (p->c0 >> 3) & 0xfc; q.g0 |= q.g0 >> 6;
    q.b0 = (p->c0 << 3) & 0xf8; q.b0 |= q.b0 >> 5;
    q.a0 = p->a0;
    q.r1 = (p->c1 >> 8) & 0xf8; q.r1 |= q.r1 >> 5;
    q.g1 = (p->c1 >> 3) & 0xfc; q.g1 |= q.g1 >> 6;
    q.b1 = (p->c1 << 3) & 0xf8; q.b1 |= q.b1 >> 5;
    q.a1 = p->a1;
    q.inv_len2 = 0.0f;
    if (p->kind == SURF_PAINT_LINEAR) {
        q.gx = p->x0 / 65536.0f;
        q.gy = p->y0 / 65536.0f;
        q.gdx = p->x1 / 65536.0f - q.gx;
        q.gdy = p->y1 / 65536.0f - q.gy;
        float len2 = q.gdx * q.gdx + q.gdy * q.gdy;
        if (len2 > 1e-9f)
            q.inv_len2 = 1.0f / len2;
    }
    return q;
}

static void paint_at(const sh_paint *q, float x, float y,
                     int *r, int *g, int *b, int *a)
{
    if (q->inv_len2 == 0.0f) {
        *r = q->r0; *g = q->g0; *b = q->b0; *a = q->a0;
        return;
    }
    float t = ((x - q->gx) * q->gdx + (y - q->gy) * q->gdy) * q->inv_len2;
    if (t < 0.0f) t = 0.0f;
    if (t > 1.0f) t = 1.0f;
    *r = q->r0 + (int)((q->r1 - q->r0) * t);
    *g = q->g0 + (int)((q->g1 - q->g0) * t);
    *b = q->b0 + (int)((q->b1 - q->b0) * t);
    *a = q->a0 + (int)((q->a1 - q->a0) * t);
}

/* ---- rasterize + composite ---- */

typedef struct { float x; int dir; } sh_cross;

static void composite_row(surf_image *dst, int y, const uint16_t *cov,
                          int xa, int xb, const sh_paint *q)
{
    for (int x = xa; x <= xb; x++) {
        int c = cov[x];
        if (!c)
            continue;
        if (c > 255) c = 255;
        int r, g, b, a;
        paint_at(q, x + 0.5f, y + 0.5f, &r, &g, &b, &a);
        a = a * c / 255;
        if (!a)
            continue;
        if (dst->format == SURF_FMT_RGB565) {
            uint16_t *d = (uint16_t *)((uint8_t *)dst->pixels +
                                       y * dst->stride) + x;
            int dr = ((*d >> 8) & 0xf8) | (*d >> 13);
            int dg = ((*d >> 3) & 0xfc) | ((*d >> 9) & 0x03);
            int db = ((*d << 3) & 0xf8) | ((*d >> 2) & 0x07);
            int nr = (r * a + dr * (255 - a) + 127) / 255;
            int ng = (g * a + dg * (255 - a) + 127) / 255;
            int nb = (b * a + db * (255 - a) + 127) / 255;
            *d = (uint16_t)(((nr & 0xf8) << 8) | ((ng & 0xfc) << 3) | (nb >> 3));
        } else if (dst->format == SURF_FMT_ARGB8888) {
            uint32_t *d = (uint32_t *)((uint8_t *)dst->pixels +
                                       y * dst->stride) + x;
            int da = *d >> 24;
            int dr = (*d >> 16) & 0xff, dg = (*d >> 8) & 0xff, db = *d & 0xff;
            int oa = a + da * (255 - a) / 255;
            int orr = (r * a + dr * da * (255 - a) / 255) / (oa ? oa : 1);
            int og = (g * a + dg * da * (255 - a) / 255) / (oa ? oa : 1);
            int ob = (b * a + db * da * (255 - a) / 255) / (oa ? oa : 1);
            *d = ((uint32_t)oa << 24) | ((uint32_t)orr << 16) |
                 ((uint32_t)og << 8) | (uint32_t)ob;
        } else {  /* A8 mask: coverage lands in alpha; color is .tint later */
            uint8_t *d = (uint8_t *)dst->pixels + y * dst->stride + x;
            *d = (uint8_t)(a + *d * (255 - a) / 255);
        }
    }
}

static int cross_cmp(const void *pa, const void *pb)
{
    float d = ((const sh_cross *)pa)->x - ((const sh_cross *)pb)->x;
    return d < 0 ? -1 : d > 0 ? 1 : 0;
}

static void raster(surf_image *dst, sh_path *p, const surf_paint *paint)
{
    if (!dst || !dst->pixels || !p->n)
        return;
    int y0 = (int)floorf(p->miny);
    int y1 = (int)ceilf(p->maxy);
    if (y0 < 0) y0 = 0;
    if (y1 > dst->h) y1 = dst->h;
    if (y0 >= y1)
        return;
    uint16_t *cov = malloc((size_t)dst->w * sizeof *cov);
    sh_cross *cr = malloc((size_t)p->n * sizeof *cr);
    if (!cov || !cr) {
        free(cov);
        free(cr);
        return;
    }
    sh_paint q = paint_prep(paint);

    for (int y = y0; y < y1; y++) {
        memset(cov, 0, (size_t)dst->w * sizeof *cov);
        int row_xa = dst->w, row_xb = -1;
        for (int s = 0; s < 4; s++) {
            float sy = y + (s + 0.5f) / 4.0f;
            int nc = 0;
            for (int i = 0; i < p->n; i++) {
                const sh_edge *e = &p->e[i];
                float ey0 = e->y0, ey1 = e->y1;
                int dir = 1;
                float ex0 = e->x0, ex1 = e->x1;
                if (ey0 > ey1) {
                    float t;
                    t = ey0; ey0 = ey1; ey1 = t;
                    t = ex0; ex0 = ex1; ex1 = t;
                    dir = -1;
                }
                if (sy < ey0 || sy >= ey1)
                    continue;
                float t = (sy - ey0) / (ey1 - ey0);
                cr[nc++] = (sh_cross){ex0 + t * (ex1 - ex0), dir};
            }
            if (!nc)
                continue;
            qsort(cr, (size_t)nc, sizeof *cr, cross_cmp);
            int wind = 0;
            float sx = 0.0f;
            for (int i = 0; i < nc; i++) {
                if (wind == 0)
                    sx = cr[i].x;
                wind += cr[i].dir;
                if (wind != 0 || cr[i].x <= sx)
                    continue;
                /* span [sx, cr[i].x): add this subsample's coverage */
                float xa = sx, xb = cr[i].x;
                if (xa < 0.0f) xa = 0.0f;
                if (xb > (float)dst->w) xb = (float)dst->w;
                if (xa >= xb)
                    continue;
                int ia = (int)xa, ib = (int)xb;
                if (ib >= dst->w) ib = dst->w - 1;
                if (ia < row_xa) row_xa = ia;
                if (ib > row_xb) row_xb = ib;
                if (ia == ib) {
                    cov[ia] += (uint16_t)((xb - xa) * 64.0f);
                } else {
                    cov[ia] += (uint16_t)((1.0f - (xa - ia)) * 64.0f);
                    for (int x = ia + 1; x < ib; x++)
                        cov[x] += 64;
                    float fb = xb - ib;
                    if (fb > 0.0f)
                        cov[ib] += (uint16_t)(fb * 64.0f);
                    else if (ib > row_xb - 1)
                        row_xb = ib;
                }
            }
        }
        if (row_xb >= row_xa)
            composite_row(dst, y, cov, row_xa, row_xb, &q);
    }
    free(cov);
    free(cr);
}

/* ---- public ops (coords Q16 pixels) ---- */

#define Q(v) ((float)(v) / 65536.0f)

void surf_image_poly(surf_image *dst, const int32_t *xy_q16, int n,
                     const surf_paint *paint)
{
    if (!dst || !xy_q16 || n < 3)
        return;
    sh_path p = {0};
    float *xy = malloc((size_t)n * 2 * sizeof *xy);
    if (xy) {
        for (int i = 0; i < n * 2; i++)
            xy[i] = Q(xy_q16[i]);
        if (path_poly(&p, xy, n))
            raster(dst, &p, paint);
    }
    free(xy);
    free(p.e);
}

void surf_image_polyline(surf_image *dst, const int32_t *xy_q16, int n,
                         int32_t width_q16, const surf_paint *paint)
{
    if (!dst || !xy_q16 || n < 1 || width_q16 <= 0)
        return;
    sh_path p = {0};
    float *xy = malloc((size_t)n * 2 * sizeof *xy);
    if (xy) {
        for (int i = 0; i < n * 2; i++)
            xy[i] = Q(xy_q16[i]);
        if (path_stroke(&p, xy, n, Q(width_q16) * 0.5f))
            raster(dst, &p, paint);
    }
    free(xy);
    free(p.e);
}

void surf_image_ellipse(surf_image *dst, int32_t cx_q16, int32_t cy_q16,
                        int32_t rx_q16, int32_t ry_q16,
                        int32_t width_q16, const surf_paint *paint)
{
    if (!dst || rx_q16 <= 0 || ry_q16 <= 0)
        return;
    float cx = Q(cx_q16), cy = Q(cy_q16), rx = Q(rx_q16), ry = Q(ry_q16);
    float rmax = rx > ry ? rx : ry;
    int segs = (int)(rmax * 0.9f);
    if (segs < 12) segs = 12;
    if (segs > 128) segs = 128;
    float *xy = malloc((size_t)(segs + 1) * 2 * sizeof *xy);
    if (!xy)
        return;
    for (int i = 0; i < segs; i++) {
        float a = (float)i * 2.0f * (float)M_PI / segs;
        xy[i * 2] = cx + rx * cosf(a);
        xy[i * 2 + 1] = cy + ry * sinf(a);
    }
    sh_path p = {0};
    bool ok;
    if (width_q16 > 0) {  /* outline: stroke the closed rim */
        xy[segs * 2] = xy[0];
        xy[segs * 2 + 1] = xy[1];
        ok = path_stroke(&p, xy, segs + 1, Q(width_q16) * 0.5f);
    } else {
        ok = path_poly(&p, xy, segs);
    }
    if (ok)
        raster(dst, &p, paint);
    free(xy);
    free(p.e);
}

/* cubic bezier stroked as a flattened polyline (pass the quadratic's
 * control point twice, weighted — the binding does the elevation) */
void surf_image_bezier(surf_image *dst, const int32_t xy_q16[8],
                       int32_t width_q16, const surf_paint *paint)
{
    if (!dst || !xy_q16 || width_q16 <= 0)
        return;
    float x0 = Q(xy_q16[0]), y0 = Q(xy_q16[1]);
    float x1 = Q(xy_q16[2]), y1 = Q(xy_q16[3]);
    float x2 = Q(xy_q16[4]), y2 = Q(xy_q16[5]);
    float x3 = Q(xy_q16[6]), y3 = Q(xy_q16[7]);
    enum { SEGS = 24 };
    float xy[(SEGS + 1) * 2];
    for (int i = 0; i <= SEGS; i++) {
        float t = (float)i / SEGS, u = 1.0f - t;
        float b0 = u * u * u, b1 = 3 * u * u * t, b2 = 3 * u * t * t,
              b3 = t * t * t;
        xy[i * 2] = b0 * x0 + b1 * x1 + b2 * x2 + b3 * x3;
        xy[i * 2 + 1] = b0 * y0 + b1 * y1 + b2 * y2 + b3 * y3;
    }
    sh_path p = {0};
    if (path_stroke(&p, xy, SEGS + 1, Q(width_q16) * 0.5f))
        raster(dst, &p, paint);
    free(p.e);
}
