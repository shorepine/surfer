/* Low-poly glTF (.glb) meshes, software-rendered into a surf_image.
 *
 * This is the software-renderer case surf_image_new_fast and
 * surf_image_flush were built for: a caller renders into an Image on its
 * OWN call (an app's frame, never the compose path), damages the sprite
 * showing it, and the compositor scales the result — so the right shape
 * on the P4 is to render SMALL and let the PPA do the enlarging.
 *
 * Scope, decided by what low-poly art actually is (measured against
 * Kenney's starter-kit GLBs, UnityGLTF export): GLB container, meshes of
 * mode-4 triangles, float POSITION, u8/u16/u32 indices, node TRS or
 * matrix transforms flattened at load, materials with baseColorFactor
 * and/or a baseColorTexture, optional COLOR_0. By default every
 * triangle gets ONE color, sampled at its UV centroid AT LOAD —
 * flat-shaded low-poly art keeps each face inside one flat region of a
 * palette texture, so the centroid is exact and the texture is FREED
 * after load: what survives is ~20 bytes a triangle, not a sampler in
 * the inner loop. SURF_MESH_TEXTURED is the other bargain, for art
 * whose detail lives INSIDE a face (a painted model, not a palette):
 * the decoded textures stay resident, per-corner UVs survive, and the
 * rasterizer samples per pixel — nearest texel, affine UV, which at
 * MESH_CAM's mild perspective is under a texel of error on low-poly
 * faces. A face with no texture renders flat either way.
 *
 * Rendering is a z-buffered scanline fill, flat color per face, one
 * directional light. The light factor is gamma-lifted before it
 * multiplies the color: the bytes are sRGB-encoded, and sRGB is near
 * enough a pure 2.2 power that correcting the FACTOR equals lighting
 * in linear — encode(decode(c)*l) == c * l^(1/2.2). Without that a
 * mid-lit face darkens perceptually about twice what the light says,
 * which is why every model here used to read darker than the same file
 * in any correct viewer. Depth interpolates 1/(D - z), which is affine
 * in screen space under perspective, so intersecting geometry (a wheel
 * through a car body) sorts per pixel and correctly. Float math and
 * malloc are fine here for shape.c's reason: this runs on the caller's
 * event, and the P4 has an FPU. The per-pixel loop is a shift, a
 * compare and two stores.
 *
 * Nothing here validates glTF beyond what it reads, but everything it
 * reads is BOUNDS-CHECKED against the chunk it came from — a .glb off
 * TULIP WORLD is bytes off the network, and an accessor pointing past
 * the buffer must be a load error, not a read. */
#include <math.h>
#ifndef M_PI    /* strict C11 on glibc and MinGW hide it; macOS does not */
#define M_PI 3.14159265358979323846
#endif
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "surf_internal.h"

/* ---- minimal JSON tokenizer (jsmn's shape: offsets, no copies) ---- */

enum { JT_OBJ = 1, JT_ARR, JT_STR, JT_PRIM };

typedef struct {
    uint8_t type;
    int32_t start, end;   /* byte offsets into the JSON chunk */
    int32_t size;         /* direct children (keys for an object) */
    int32_t parent;
} jtok;

typedef struct {
    const char *js;
    int32_t     jslen;
    jtok       *t;
    int         n;
    const uint8_t *bin;
    uint32_t    binlen;
} gctx;

static int j_parse(const char *js, int32_t len, jtok *t, int cap)
{
    int n = 0, super = -1;
    for (int32_t pos = 0; pos < len; pos++) {
        char c = js[pos];
        if (c == '{' || c == '[') {
            if (n >= cap)
                return -1;
            t[n] = (jtok){c == '{' ? JT_OBJ : JT_ARR, pos, -1, 0, super};
            if (super >= 0)
                t[super].size++;
            super = n++;
        } else if (c == '}' || c == ']') {
            uint8_t type = c == '}' ? JT_OBJ : JT_ARR;
            if (super < 0 || t[super].type != type)
                return -1;
            t[super].end = pos + 1;
            super = t[super].parent;
            /* a container that was a key's value: the key is spent */
            if (super >= 0 && t[super].type == JT_STR)
                super = t[super].parent;
        } else if (c == '"') {
            int32_t s = pos + 1;
            pos++;
            while (pos < len && js[pos] != '"') {
                if (js[pos] == '\\')
                    pos++;
                pos++;
            }
            if (pos >= len || n >= cap)
                return -1;
            t[n] = (jtok){JT_STR, s, pos, 0, super};
            if (super >= 0)
                t[super].size++;
            if (super >= 0 && t[super].type == JT_OBJ) {
                super = n;           /* a key: the next value is its child */
            } else if (super >= 0 && t[super].type == JT_STR) {
                super = t[super].parent;   /* a key's string value */
            }
            n++;
        } else if (c == '-' || (c >= '0' && c <= '9') ||
                   c == 't' || c == 'f' || c == 'n') {
            int32_t s = pos;
            while (pos < len && js[pos] != ',' && js[pos] != '}' &&
                   js[pos] != ']' && js[pos] != ' ' && js[pos] != '\t' &&
                   js[pos] != '\r' && js[pos] != '\n')
                pos++;
            if (n >= cap)
                return -1;
            t[n] = (jtok){JT_PRIM, s, pos, 0, super};
            if (super >= 0)
                t[super].size++;
            if (super >= 0 && t[super].type == JT_STR)
                super = t[super].parent;
            n++;
            pos--;
        }
        /* ':', ',' and whitespace carry no information the tokens lack */
    }
    return super == -1 ? n : -1;
}

/* index just past token i's whole subtree */
static int j_skip(const gctx *g, int i)
{
    int32_t end = g->t[i].end;
    int j = i + 1;
    while (j < g->n && g->t[j].start < end)
        j++;
    return j;
}

static bool j_streq(const gctx *g, int i, const char *s)
{
    int32_t l = g->t[i].end - g->t[i].start;
    return (int32_t)strlen(s) == l &&
           memcmp(g->js + g->t[i].start, s, (size_t)l) == 0;
}

/* value token for a key in an object, -1 when absent */
static int j_get(const gctx *g, int obj, const char *key)
{
    if (obj < 0 || g->t[obj].type != JT_OBJ)
        return -1;
    int i = obj + 1;
    for (int32_t k = 0; k < g->t[obj].size; k++) {
        int val = i + 1;                 /* the key's one child */
        if (j_streq(g, i, key))
            return val < g->n ? val : -1;
        i = j_skip(g, val);
    }
    return -1;
}

static int j_at(const gctx *g, int arr, int idx)
{
    if (arr < 0 || idx < 0 || g->t[arr].type != JT_ARR ||
        idx >= g->t[arr].size)
        return -1;
    int i = arr + 1;
    for (int k = 0; k < idx; k++)
        i = j_skip(g, i);
    return i;
}

static double j_num(const gctx *g, int i, double dflt)
{
    if (i < 0 || g->t[i].type != JT_PRIM)
        return dflt;
    /* the token is always followed by a delimiter inside the chunk, so
     * strtod stops on its own; GLB pads the JSON chunk with spaces */
    return strtod(g->js + g->t[i].start, NULL);
}

static int j_int(const gctx *g, int i, int dflt)
{
    return i < 0 ? dflt : (int)j_num(g, i, dflt);
}

/* ---- accessors ---- */

static int comp_size(int ctype)
{
    switch (ctype) {
    case 5120: case 5121: return 1;   /* byte / ubyte */
    case 5122: case 5123: return 2;   /* short / ushort */
    case 5125: case 5126: return 4;   /* uint / float */
    default: return 0;
    }
}

typedef struct {
    const uint8_t *p;
    int count, ctype, ncomp, stride;
} garr;

/* resolve accessor index -> bounds-checked view into the bin chunk */
static bool acc_view(const gctx *g, int accessors, int bufviews, int idx,
                     garr *out)
{
    int a = j_at(g, accessors, idx);
    if (a < 0)
        return false;
    int bv = j_at(g, bufviews, j_int(g, j_get(g, a, "bufferView"), -1));
    if (bv < 0)
        return false;
    out->count = j_int(g, j_get(g, a, "count"), 0);
    out->ctype = j_int(g, j_get(g, a, "componentType"), 0);
    int tt = j_get(g, a, "type");
    out->ncomp = tt < 0 ? 0
               : j_streq(g, tt, "SCALAR") ? 1
               : j_streq(g, tt, "VEC2") ? 2
               : j_streq(g, tt, "VEC3") ? 3
               : j_streq(g, tt, "VEC4") ? 4 : 0;
    int cs = comp_size(out->ctype);
    if (!cs || !out->ncomp || out->count <= 0)
        return false;
    int packed = cs * out->ncomp;
    out->stride = j_int(g, j_get(g, bv, "byteStride"), packed);
    if (out->stride < packed)
        return false;
    uint32_t bvoff = (uint32_t)j_int(g, j_get(g, bv, "byteOffset"), 0);
    uint32_t bvlen = (uint32_t)j_int(g, j_get(g, bv, "byteLength"), 0);
    uint32_t aoff = (uint32_t)j_int(g, j_get(g, a, "byteOffset"), 0);
    uint64_t need = (uint64_t)aoff +
                    (uint64_t)(out->count - 1) * out->stride + packed;
    if (!g->bin || (uint64_t)bvoff + bvlen > g->binlen || need > bvlen)
        return false;
    out->p = g->bin + bvoff + aoff;
    return true;
}

static void acc_vec3f(const garr *a, int i, float out[3])
{
    memcpy(out, a->p + (size_t)i * a->stride, 12);
}

static void acc_vec2f(const garr *a, int i, float out[2])
{
    memcpy(out, a->p + (size_t)i * a->stride, 8);
}

static uint32_t acc_index(const garr *a, int i)
{
    const uint8_t *p = a->p + (size_t)i * a->stride;
    switch (a->ctype) {
    case 5121: return *p;
    case 5123: { uint16_t v; memcpy(&v, p, 2); return v; }
    case 5125: { uint32_t v; memcpy(&v, p, 4); return v; }
    default: return 0xffffffffu;
    }
}

/* COLOR_0 component -> 0..1 (float, or normalized u8/u16) */
static float acc_colorf(const garr *a, int i, int comp)
{
    const uint8_t *p = a->p + (size_t)i * a->stride;
    switch (a->ctype) {
    case 5126: { float v; memcpy(&v, p + comp * 4, 4); return v; }
    case 5121: return p[comp] / 255.0f;
    case 5123: { uint16_t v; memcpy(&v, p + comp * 2, 2); return v / 65535.0f; }
    default: return 1.0f;
    }
}

/* ---- 3x4 transforms (row-major; last column is translation) ---- */

static void mat_identity(float m[12])
{
    memset(m, 0, 12 * sizeof(float));
    m[0] = m[5] = m[10] = 1.0f;
}

static void mat_mul(const float a[12], const float b[12], float out[12])
{
    for (int r = 0; r < 3; r++) {
        for (int c = 0; c < 3; c++)
            out[r * 4 + c] = a[r * 4] * b[c] + a[r * 4 + 1] * b[4 + c] +
                             a[r * 4 + 2] * b[8 + c];
        out[r * 4 + 3] = a[r * 4] * b[3] + a[r * 4 + 1] * b[7] +
                         a[r * 4 + 2] * b[11] + a[r * 4 + 3];
    }
}

static void mat_apply(const float m[12], const float v[3], float out[3])
{
    for (int r = 0; r < 3; r++)
        out[r] = m[r * 4] * v[0] + m[r * 4 + 1] * v[1] +
                 m[r * 4 + 2] * v[2] + m[r * 4 + 3];
}

/* node-local transform: `matrix` (column-major 4x4) or T * R * S */
static void node_local(const gctx *g, int node, float m[12])
{
    int mt = j_get(g, node, "matrix");
    if (mt >= 0) {
        float c[16];
        for (int i = 0; i < 16; i++)
            c[i] = (float)j_num(g, j_at(g, mt, i), i % 5 == 0 ? 1.0 : 0.0);
        for (int r = 0; r < 3; r++)
            for (int col = 0; col < 4; col++)
                m[r * 4 + col] = c[col * 4 + r];
        return;
    }
    float tx = 0, ty = 0, tz = 0, sx = 1, sy = 1, sz = 1;
    float qx = 0, qy = 0, qz = 0, qw = 1;
    int t = j_get(g, node, "translation");
    if (t >= 0) {
        tx = (float)j_num(g, j_at(g, t, 0), 0);
        ty = (float)j_num(g, j_at(g, t, 1), 0);
        tz = (float)j_num(g, j_at(g, t, 2), 0);
    }
    int s = j_get(g, node, "scale");
    if (s >= 0) {
        sx = (float)j_num(g, j_at(g, s, 0), 1);
        sy = (float)j_num(g, j_at(g, s, 1), 1);
        sz = (float)j_num(g, j_at(g, s, 2), 1);
    }
    int r = j_get(g, node, "rotation");
    if (r >= 0) {
        qx = (float)j_num(g, j_at(g, r, 0), 0);
        qy = (float)j_num(g, j_at(g, r, 1), 0);
        qz = (float)j_num(g, j_at(g, r, 2), 0);
        qw = (float)j_num(g, j_at(g, r, 3), 1);
    }
    float xx = qx * qx, yy = qy * qy, zz = qz * qz;
    float xy = qx * qy, xz = qx * qz, yz = qy * qz;
    float wx = qw * qx, wy = qw * qy, wz = qw * qz;
    float rm[9] = {
        1 - 2 * (yy + zz), 2 * (xy - wz),     2 * (xz + wy),
        2 * (xy + wz),     1 - 2 * (xx + zz), 2 * (yz - wx),
        2 * (xz - wy),     2 * (yz + wx),     1 - 2 * (xx + yy),
    };
    float sc[3] = {sx, sy, sz};
    for (int row = 0; row < 3; row++) {
        for (int col = 0; col < 3; col++)
            m[row * 4 + col] = rm[row * 3 + col] * sc[col];
        m[row * 4 + 3] = row == 0 ? tx : row == 1 ? ty : tz;
    }
}

/* ---- the mesh ---- */

struct surf_mesh {
    float    *vtx;    /* 3 floats per vertex: centered, radius-1 model */
    int       nvtx;
    uint16_t *idx;    /* 3 per triangle */
    uint8_t  *clr;    /* 3 bytes per triangle: base RGB — the flat sample,
                         or the baseColorFactor for a per-pixel face */
    int8_t   *nrm;    /* 3 per triangle, face normal * 127 */
    uint8_t  *side;   /* 1 per triangle: bit0 = material was doubleSided */
    int       ntri;
    /* SURF_MESH_TEXTURED only; NULL/0 on a flat-shaded load */
    float    *tuv;    /* 6 per triangle: u,v per corner, transform applied */
    int16_t  *tmap;   /* 1 per triangle: index into timg, -1 = flat face */
    surf_image **timg;/* resident decoded textures, owned by the mesh */
    int       ntimg;
};

typedef struct {
    struct surf_mesh m;
    int vcap, tcap;
    bool textured;
} mbuild;

static bool mb_room(mbuild *b, int verts, int tris)
{
    if (b->m.nvtx + verts > b->vcap) {
        int cap = b->vcap ? b->vcap : 256;
        while (cap < b->m.nvtx + verts)
            cap *= 2;
        float *v = realloc(b->m.vtx, (size_t)cap * 3 * sizeof(float));
        if (!v)
            return false;
        b->m.vtx = v;
        b->vcap = cap;
    }
    if (b->m.ntri + tris > b->tcap) {
        int cap = b->tcap ? b->tcap : 256;
        while (cap < b->m.ntri + tris)
            cap *= 2;
        uint16_t *ix = realloc(b->m.idx, (size_t)cap * 3 * sizeof(uint16_t));
        uint8_t *cl = ix ? realloc(b->m.clr, (size_t)cap * 3) : NULL;
        int8_t *nm = cl ? realloc(b->m.nrm, (size_t)cap * 3) : NULL;
        uint8_t *sd = nm ? realloc(b->m.side, (size_t)cap) : NULL;
        if (ix) b->m.idx = ix;
        if (cl) b->m.clr = cl;
        if (nm) b->m.nrm = nm;
        if (sd) b->m.side = sd;
        if (!sd)
            return false;
        if (b->textured) {
            float *uv = realloc(b->m.tuv, (size_t)cap * 6 * sizeof(float));
            int16_t *tm = uv ? realloc(b->m.tmap,
                                       (size_t)cap * sizeof(int16_t)) : NULL;
            if (uv) b->m.tuv = uv;
            if (tm) b->m.tmap = tm;
            if (!tm)
                return false;
        }
        b->tcap = cap;
    }
    return true;
}

/* per-material answers, resolved once */
typedef struct {
    float r, g, b;        /* baseColorFactor */
    int   image;          /* glTF image index, -1 for none */
    float uo, vo, us, vs; /* KHR_texture_transform offset/scale */
    bool  twoside;
} gmat;

static void mat_resolve(const gctx *g, int materials, int textures, int idx,
                        gmat *out)
{
    *out = (gmat){1, 1, 1, -1, 0, 0, 1, 1, false};
    int m = j_at(g, materials, idx);
    if (m < 0)
        return;
    int ds = j_get(g, m, "doubleSided");
    out->twoside = ds >= 0 && j_streq(g, ds, "true");
    int pbr = j_get(g, m, "pbrMetallicRoughness");
    if (pbr < 0)
        return;
    int f = j_get(g, pbr, "baseColorFactor");
    if (f >= 0) {
        out->r = (float)j_num(g, j_at(g, f, 0), 1);
        out->g = (float)j_num(g, j_at(g, f, 1), 1);
        out->b = (float)j_num(g, j_at(g, f, 2), 1);
    }
    int bct = j_get(g, pbr, "baseColorTexture");
    if (bct < 0)
        return;
    int tex = j_at(g, textures, j_int(g, j_get(g, bct, "index"), -1));
    if (tex >= 0)
        out->image = j_int(g, j_get(g, tex, "source"), -1);
    int xt = j_get(g, j_get(g, bct, "extensions"), "KHR_texture_transform");
    if (xt >= 0) {
        int off = j_get(g, xt, "offset");
        out->uo = (float)j_num(g, j_at(g, off, 0), 0);
        out->vo = (float)j_num(g, j_at(g, off, 1), 0);
        int sc = j_get(g, xt, "scale");
        out->us = (float)j_num(g, j_at(g, sc, 0), 1);
        out->vs = (float)j_num(g, j_at(g, sc, 1), 1);
    }
}

/* nearest-texel sample of a decoded ARGB image, repeat wrap */
static void tex_sample(const surf_image *t, float u, float v, float rgb[3])
{
    u -= floorf(u);
    v -= floorf(v);
    int x = (int)(u * t->w);
    int y = (int)(v * t->h);
    if (x >= t->w) x = t->w - 1;
    if (y >= t->h) y = t->h - 1;
    uint32_t p = *(const uint32_t *)((const uint8_t *)t->pixels +
                                     (size_t)y * t->stride + (size_t)x * 4);
    rgb[0] = ((p >> 16) & 0xff) / 255.0f;
    rgb[1] = ((p >> 8) & 0xff) / 255.0f;
    rgb[2] = (p & 0xff) / 255.0f;
}

static void mesh_err(char *err, size_t cap, const char *msg)
{
    if (err && cap)
        snprintf(err, cap, "%s", msg);
}

void surf_mesh_destroy(surf_mesh *m)
{
    if (!m)
        return;
    free(m->vtx);
    free(m->idx);
    free(m->clr);
    free(m->nrm);
    free(m->side);
    free(m->tuv);
    free(m->tmap);
    for (int i = 0; i < m->ntimg; i++)
        if (m->timg && m->timg[i])
            surf_image_destroy(m->timg[i]);
    free(m->timg);
    free(m);
}

int surf_mesh_tris(const surf_mesh *m)
{
    return m ? m->ntri : 0;
}

/* walk a node subtree, appending every mesh primitive it reaches.
 * Depth-capped: glTF is a tree, but these are bytes off a network. */
static bool flatten_node(const gctx *g, int nodes, int meshes, int accessors,
                         int bufviews, int node_idx, const float parent[12],
                         mbuild *b, const gmat *mats, int nmat,
                         surf_image **timgs, int depth, char *err, size_t ecap)
{
    if (depth > 32)
        return true;
    int node = j_at(g, nodes, node_idx);
    if (node < 0)
        return true;
    float local[12], world[12];
    node_local(g, node, local);
    mat_mul(parent, local, world);

    int mesh_i = j_int(g, j_get(g, node, "mesh"), -1);
    if (mesh_i >= 0) {
        int mesh = j_at(g, meshes, mesh_i);
        int prims = j_get(g, mesh, "primitives");
        for (int pi = 0; pi < (prims >= 0 ? g->t[prims].size : 0); pi++) {
            int prim = j_at(g, prims, pi);
            if (j_int(g, j_get(g, prim, "mode"), 4) != 4)
                continue;                       /* triangles only */
            int attrs = j_get(g, prim, "attributes");
            garr pos = {0}, uv = {0}, vc = {0}, ind = {0};
            bool has_uv = false, has_vc = false, has_ind = false;
            if (!acc_view(g, accessors, bufviews,
                          j_int(g, j_get(g, attrs, "POSITION"), -1), &pos) ||
                pos.ctype != 5126 || pos.ncomp != 3) {
                mesh_err(err, ecap, "bad POSITION accessor");
                return false;
            }
            has_uv = acc_view(g, accessors, bufviews,
                              j_int(g, j_get(g, attrs, "TEXCOORD_0"), -1),
                              &uv) && uv.ctype == 5126 && uv.ncomp == 2;
            has_vc = acc_view(g, accessors, bufviews,
                              j_int(g, j_get(g, attrs, "COLOR_0"), -1),
                              &vc) && vc.ncomp >= 3;
            has_ind = acc_view(g, accessors, bufviews,
                               j_int(g, j_get(g, prim, "indices"), -1), &ind);
            int nidx = has_ind ? ind.count : pos.count;
            int ntri = nidx / 3;
            if (b->m.nvtx + pos.count > 65535) {
                mesh_err(err, ecap, "mesh too big (>64k vertices)");
                return false;
            }
            if (!mb_room(b, pos.count, ntri)) {
                mesh_err(err, ecap, "out of memory");
                return false;
            }
            int base = b->m.nvtx;
            for (int i = 0; i < pos.count; i++) {
                float v[3];
                acc_vec3f(&pos, i, v);
                mat_apply(world, v, &b->m.vtx[(size_t)(base + i) * 3]);
            }
            b->m.nvtx += pos.count;

            int mat_i = j_int(g, j_get(g, prim, "material"), -1);
            const gmat *mat = (mat_i >= 0 && mat_i < nmat) ? &mats[mat_i]
                                                           : NULL;
            const surf_image *timg =
                (mat && mat->image >= 0 && has_uv) ? timgs[mat->image] : NULL;

            for (int t = 0; t < ntri; t++) {
                uint32_t i0, i1, i2;
                if (has_ind) {
                    i0 = acc_index(&ind, t * 3);
                    i1 = acc_index(&ind, t * 3 + 1);
                    i2 = acc_index(&ind, t * 3 + 2);
                } else {
                    i0 = (uint32_t)t * 3;
                    i1 = i0 + 1;
                    i2 = i0 + 2;
                }
                if (i0 >= (uint32_t)pos.count || i1 >= (uint32_t)pos.count ||
                    i2 >= (uint32_t)pos.count)
                    continue;                    /* hostile index: drop it */
                int ti = b->m.ntri;
                b->m.idx[ti * 3] = (uint16_t)(base + i0);
                b->m.idx[ti * 3 + 1] = (uint16_t)(base + i1);
                b->m.idx[ti * 3 + 2] = (uint16_t)(base + i2);

                /* face normal from the WORLD-space triangle: always
                 * consistent with the vertices actually rendered, and
                 * no NORMAL accessor to trust */
                const float *v0 = &b->m.vtx[(size_t)(base + i0) * 3];
                const float *v1 = &b->m.vtx[(size_t)(base + i1) * 3];
                const float *v2 = &b->m.vtx[(size_t)(base + i2) * 3];
                float e1[3] = {v1[0] - v0[0], v1[1] - v0[1], v1[2] - v0[2]};
                float e2[3] = {v2[0] - v0[0], v2[1] - v0[1], v2[2] - v0[2]};
                float n[3] = {e1[1] * e2[2] - e1[2] * e2[1],
                              e1[2] * e2[0] - e1[0] * e2[2],
                              e1[0] * e2[1] - e1[1] * e2[0]};
                float nl = sqrtf(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]);
                if (nl < 1e-12f)
                    nl = 1.0f;
                b->m.nrm[ti * 3] = (int8_t)(n[0] / nl * 127.0f);
                b->m.nrm[ti * 3 + 1] = (int8_t)(n[1] / nl * 127.0f);
                b->m.nrm[ti * 3 + 2] = (int8_t)(n[2] / nl * 127.0f);

                float rgb[3] = {1, 1, 1};
                if (b->textured) {
                    b->m.tmap[ti] = -1;
                    memset(&b->m.tuv[(size_t)ti * 6], 0, 6 * sizeof(float));
                }
                if (timg && b->textured) {
                    /* per-pixel face: keep the corner UVs (texture
                     * transform applied HERE, so render needs no
                     * material) and leave rgb the baseColorFactor */
                    float uv0[2], uv1[2], uv2[2];
                    acc_vec2f(&uv, (int)i0, uv0);
                    acc_vec2f(&uv, (int)i1, uv1);
                    acc_vec2f(&uv, (int)i2, uv2);
                    float *tp = &b->m.tuv[(size_t)ti * 6];
                    tp[0] = mat->uo + uv0[0] * mat->us;
                    tp[1] = mat->vo + uv0[1] * mat->vs;
                    tp[2] = mat->uo + uv1[0] * mat->us;
                    tp[3] = mat->vo + uv1[1] * mat->vs;
                    tp[4] = mat->uo + uv2[0] * mat->us;
                    tp[5] = mat->vo + uv2[1] * mat->vs;
                    b->m.tmap[ti] = (int16_t)mat->image;
                } else if (timg) {
                    float uv0[2], uv1[2], uv2[2];
                    acc_vec2f(&uv, (int)i0, uv0);
                    acc_vec2f(&uv, (int)i1, uv1);
                    acc_vec2f(&uv, (int)i2, uv2);
                    float cu = (uv0[0] + uv1[0] + uv2[0]) / 3.0f;
                    float cv = (uv0[1] + uv1[1] + uv2[1]) / 3.0f;
                    tex_sample(timg, mat->uo + cu * mat->us,
                               mat->vo + cv * mat->vs, rgb);
                } else if (has_vc) {
                    for (int c = 0; c < 3; c++)
                        rgb[c] = (acc_colorf(&vc, (int)i0, c) +
                                  acc_colorf(&vc, (int)i1, c) +
                                  acc_colorf(&vc, (int)i2, c)) / 3.0f;
                }
                if (mat) {
                    rgb[0] *= mat->r;
                    rgb[1] *= mat->g;
                    rgb[2] *= mat->b;
                }
                for (int c = 0; c < 3; c++) {
                    float x = rgb[c] * 255.0f;
                    b->m.clr[ti * 3 + c] =
                        (uint8_t)(x < 0 ? 0 : x > 255 ? 255 : x);
                }
                b->m.side[ti] = (mat && mat->twoside) ? 1 : 0;
                b->m.ntri++;
            }
        }
    }

    int kids = j_get(g, node, "children");
    for (int k = 0; k < (kids >= 0 ? g->t[kids].size : 0); k++) {
        if (!flatten_node(g, nodes, meshes, accessors, bufviews,
                          j_int(g, j_at(g, kids, k), -1), world, b, mats,
                          nmat, timgs, depth + 1, err, ecap))
            return false;
    }
    return true;
}

static uint32_t rd32(const uint8_t *p)
{
    uint32_t v;
    memcpy(&v, p, 4);
    return v;
}

static void mesh_free_fields(struct surf_mesh *m)
{
    free(m->vtx);
    free(m->idx);
    free(m->clr);
    free(m->nrm);
    free(m->side);
    free(m->tuv);
    free(m->tmap);
}

surf_mesh *surf_mesh_from_glb(const void *glb, size_t len,
                              const void *tex_png, size_t tex_len,
                              unsigned flags, char *err, size_t ecap)
{
    mesh_err(err, ecap, "");
    const uint8_t *d = glb;
    if (!d || len < 20 || rd32(d) != 0x46546c67u || rd32(d + 4) != 2) {
        mesh_err(err, ecap, "not a glTF 2 .glb");
        return NULL;
    }
    uint32_t total = rd32(d + 8);
    if (total > len)
        total = (uint32_t)len;

    /* chunk walk: one JSON, at most one BIN */
    const char *js = NULL;
    uint32_t jslen = 0;
    const uint8_t *bin = NULL;
    uint32_t binlen = 0;
    for (uint32_t off = 12; off + 8 <= total;) {
        uint32_t clen = rd32(d + off), ctype = rd32(d + off + 4);
        if (off + 8 + clen > total)
            break;
        if (ctype == 0x4e4f534au) {          /* 'JSON' */
            js = (const char *)d + off + 8;
            jslen = clen;
        } else if (ctype == 0x004e4942u) {   /* 'BIN' */
            bin = d + off + 8;
            binlen = clen;
        }
        off += 8 + clen;
    }
    if (!js) {
        mesh_err(err, ecap, "no JSON chunk");
        return NULL;
    }

    int cap = (int)(jslen / 2) + 64;
    jtok *toks = malloc((size_t)cap * sizeof *toks);
    if (!toks) {
        mesh_err(err, ecap, "out of memory");
        return NULL;
    }
    gctx g = {js, (int32_t)jslen, toks, 0, bin, binlen};
    g.n = j_parse(js, (int32_t)jslen, toks, cap);
    if (g.n <= 0 || toks[0].type != JT_OBJ) {
        free(toks);
        mesh_err(err, ecap, "bad glTF JSON");
        return NULL;
    }

    int accessors = j_get(&g, 0, "accessors");
    int bufviews = j_get(&g, 0, "bufferViews");
    int meshes = j_get(&g, 0, "meshes");
    int nodes = j_get(&g, 0, "nodes");
    int materials = j_get(&g, 0, "materials");
    int textures = j_get(&g, 0, "textures");
    int images = j_get(&g, 0, "images");
    int scenes = j_get(&g, 0, "scenes");
    if (meshes < 0 || accessors < 0 || bufviews < 0) {
        free(toks);
        mesh_err(err, ecap, "no meshes");
        return NULL;
    }

    int nmat = materials >= 0 ? g.t[materials].size : 0;
    gmat *mats = nmat ? malloc((size_t)nmat * sizeof *mats) : NULL;
    for (int i = 0; i < nmat; i++)
        mat_resolve(&g, materials, textures, i, &mats[i]);

    /* decode each referenced texture ONCE, sample at load, free below.
     * An embedded image comes from its bufferView; an external URI is
     * answered by the caller-supplied tex_png (Kenney's kits keep one
     * shared colormap.png beside the models). A texture that cannot be
     * decoded is not an error — the face keeps its baseColorFactor. */
    int nimg = images >= 0 ? g.t[images].size : 0;
    surf_image **timgs = NULL;
    if (nimg) {
        timgs = calloc((size_t)nimg, sizeof *timgs);
        if (timgs) {
            for (int mi = 0; mi < nmat; mi++) {
                int ii = mats[mi].image;
                if (ii < 0 || ii >= nimg || timgs[ii])
                    continue;
                int im = j_at(&g, images, ii);
                int bv = j_at(&g, bufviews,
                              j_int(&g, j_get(&g, im, "bufferView"), -1));
                if (bv >= 0 && bin) {
                    uint32_t o = (uint32_t)j_int(&g, j_get(&g, bv, "byteOffset"), 0);
                    uint32_t l = (uint32_t)j_int(&g, j_get(&g, bv, "byteLength"), 0);
                    if ((uint64_t)o + l <= binlen)
                        timgs[ii] = surf_image_from_png(bin + o, l);
                } else if (tex_png && tex_len) {
                    timgs[ii] = surf_image_from_png(tex_png, tex_len);
                }
            }
        }
    }

    mbuild b;
    memset(&b, 0, sizeof b);
    b.textured = (flags & SURF_MESH_TEXTURED) != 0;
    bool ok = true;

    /* default scene's roots; a file with no scene walks every node */
    int scene = j_at(&g, scenes, j_int(&g, j_get(&g, 0, "scene"), 0));
    int roots = scene >= 0 ? j_get(&g, scene, "nodes") : -1;
    float ident[12];
    mat_identity(ident);
    int nroots = roots >= 0 ? g.t[roots].size
                            : (nodes >= 0 ? g.t[nodes].size : 0);
    for (int i = 0; ok && i < nroots; i++) {
        int ni = roots >= 0 ? j_int(&g, j_at(&g, roots, i), -1) : i;
        ok = flatten_node(&g, nodes, meshes, accessors, bufviews, ni, ident,
                          &b, mats, nmat, timgs, 0, err, ecap);
    }

    free(mats);
    free(toks);

    if (ok && b.m.ntri == 0) {
        mesh_err(err, ecap, "no triangles");
        ok = false;
    }
    /* a textured load OWNS its textures from here; every other outcome
     * frees them — the flat sample already took what it needed */
    if (ok && b.textured) {
        b.m.timg = timgs;
        b.m.ntimg = timgs ? nimg : 0;
    } else {
        for (int i = 0; i < nimg; i++)
            if (timgs && timgs[i])
                surf_image_destroy(timgs[i]);
        free(timgs);
    }
    if (!ok) {
        mesh_free_fields(&b.m);
        return NULL;
    }

    /* center on the bounding box, normalize to radius 1: render scale
     * is then simply "radius in pixels", whatever units the file used */
    float lo[3] = {1e30f, 1e30f, 1e30f}, hi[3] = {-1e30f, -1e30f, -1e30f};
    for (int i = 0; i < b.m.nvtx; i++) {
        for (int c = 0; c < 3; c++) {
            float v = b.m.vtx[(size_t)i * 3 + c];
            if (v < lo[c]) lo[c] = v;
            if (v > hi[c]) hi[c] = v;
        }
    }
    float ctr[3] = {(lo[0] + hi[0]) / 2, (lo[1] + hi[1]) / 2,
                    (lo[2] + hi[2]) / 2};
    float r2max = 0;
    for (int i = 0; i < b.m.nvtx; i++) {
        float dx = b.m.vtx[(size_t)i * 3] - ctr[0];
        float dy = b.m.vtx[(size_t)i * 3 + 1] - ctr[1];
        float dz = b.m.vtx[(size_t)i * 3 + 2] - ctr[2];
        float r2 = dx * dx + dy * dy + dz * dz;
        if (r2 > r2max)
            r2max = r2;
    }
    float inv_r = r2max > 1e-20f ? 1.0f / sqrtf(r2max) : 1.0f;
    for (int i = 0; i < b.m.nvtx; i++)
        for (int c = 0; c < 3; c++)
            b.m.vtx[(size_t)i * 3 + c] =
                (b.m.vtx[(size_t)i * 3 + c] - ctr[c]) * inv_r;

    surf_mesh *m = malloc(sizeof *m);
    if (!m) {
        mesh_err(err, ecap, "out of memory");
        for (int i = 0; i < b.m.ntimg; i++)
            if (b.m.timg[i])
                surf_image_destroy(b.m.timg[i]);
        free(b.m.timg);
        mesh_free_fields(&b.m);
        return NULL;
    }
    *m = b.m;
    return m;
}

/* ---- rendering ---- */

/* scratch shared by every render call: projected vertices and the
 * z-buffer. Grown on demand, never in step with any one mesh, freed by
 * surf_mesh_reset() from surf_deinit — the ink table's lifecycle. */
static struct {
    float    *pv;     /* 3 per vertex: sx, sy, depth */
    int       vcap;
    uint16_t *z;
    int       zcap;
} ms;

void surf_mesh_reset(void)
{
    free(ms.pv);
    free(ms.z);
    memset(&ms, 0, sizeof ms);
}

/* camera distance in model radii. Close enough that spinning reads as
 * 3D, far enough that a radius-1 model projects near size_px. */
#define MESH_CAM 3.0f

/* Lambert factor -> the sRGB-space multiplier that equals lighting in
 * LINEAR (see the header comment): 256 * l^(1/2.2), l in 128ths.
 * Constant data computed on first use — nothing for surf_mesh_reset to
 * free, and a table beats a powf per face on the device's FPU. */
static uint16_t lit_gamma[129];
static bool lit_gamma_ready;

bool surf_mesh_render(const surf_mesh *m, surf_image *dst,
                      float rx_deg, float ry_deg, float rz_deg,
                      float size_px, float cx, float cy, int cull)
{
    if (!m || !m->ntri || !dst || !dst->pixels)
        return false;
    if (dst->format != SURF_FMT_RGB565 && dst->format != SURF_FMT_ARGB8888)
        return false;
    int w = dst->w, h = dst->h;
    if (ms.vcap < m->nvtx) {
        float *pv = realloc(ms.pv, (size_t)m->nvtx * 3 * sizeof(float));
        if (!pv)
            return false;
        ms.pv = pv;
        ms.vcap = m->nvtx;
    }
    if (ms.zcap < w * h) {
        uint16_t *z = realloc(ms.z, (size_t)w * h * sizeof(uint16_t));
        if (!z)
            return false;
        ms.z = z;
        ms.zcap = w * h;
    }
    memset(ms.z, 0, (size_t)w * h * sizeof(uint16_t));
    surf_ink_dirty(dst);

    if (!lit_gamma_ready) {
        for (int i = 0; i <= 128; i++)
            lit_gamma[i] = (uint16_t)(powf((float)i / 128.0f, 1.0f / 2.2f) *
                                      256.0f + 0.5f);
        lit_gamma_ready = true;
    }

    /* R = Rz * Rx * Ry: ry spins the model, rx tilts it toward the
     * viewer, rz rolls the result — the order a turntable wants */
    float ax = rx_deg * (float)M_PI / 180.0f;
    float ay = ry_deg * (float)M_PI / 180.0f;
    float az = rz_deg * (float)M_PI / 180.0f;
    float cxr = cosf(ax), sxr = sinf(ax);
    float cyr = cosf(ay), syr = sinf(ay);
    float czr = cosf(az), szr = sinf(az);
    float rxy[9] = {                       /* Rx * Ry */
        cyr,        0,    syr,
        sxr * syr,  cxr, -sxr * cyr,
        -cxr * syr, sxr,  cxr * cyr,
    };
    float R[9];
    for (int c = 0; c < 3; c++) {          /* Rz * (Rx * Ry) */
        R[c] = czr * rxy[c] - szr * rxy[3 + c];
        R[3 + c] = szr * rxy[c] + czr * rxy[3 + c];
        R[6 + c] = rxy[6 + c];
    }

    /* project: view z is toward the viewer; depth interpolates
     * q = 1/(CAM - z), which is affine in screen space, scaled so a
     * radius-1 model spans most of the u16 range */
    for (int i = 0; i < m->nvtx; i++) {
        const float *v = &m->vtx[(size_t)i * 3];
        float x = R[0] * v[0] + R[1] * v[1] + R[2] * v[2];
        float y = R[3] * v[0] + R[4] * v[1] + R[5] * v[2];
        float z = R[6] * v[0] + R[7] * v[1] + R[8] * v[2];
        float den = MESH_CAM - z;
        if (den < 0.05f)
            den = 0.05f;
        float pw = MESH_CAM / den;         /* 1.0 at the model's center */
        float *p = &ms.pv[(size_t)i * 3];
        p[0] = cx + x * size_px * pw;
        p[1] = cy - y * size_px * pw;
        p[2] = 1.0f / den;
    }

    const float lx = -0.30f, ly = 0.55f, lz = 0.78f;  /* view-space light */
    int drawn = 0;

    for (int t = 0; t < m->ntri; t++) {
        const float *p0 = &ms.pv[(size_t)m->idx[t * 3] * 3];
        const float *p1 = &ms.pv[(size_t)m->idx[t * 3 + 1] * 3];
        const float *p2 = &ms.pv[(size_t)m->idx[t * 3 + 2] * 3];
        float area2 = (p1[0] - p0[0]) * (p2[1] - p0[1]) -
                      (p2[0] - p0[0]) * (p1[1] - p0[1]);
        /* glTF front faces are CCW; the y flip makes them CLOCKWISE on
         * screen, so a front face has negative signed area here */
        bool front = area2 < 0;
        if (!front) {
            bool two = cull == 0 || (cull < 0 && (m->side[t] & 1));
            if (!two || area2 == 0)
                continue;
        }

        float nx = m->nrm[t * 3] * (1.0f / 127.0f);
        float ny = m->nrm[t * 3 + 1] * (1.0f / 127.0f);
        float nz = m->nrm[t * 3 + 2] * (1.0f / 127.0f);
        float vx = R[0] * nx + R[1] * ny + R[2] * nz;
        float vy = R[3] * nx + R[4] * ny + R[5] * nz;
        float vz = R[6] * nx + R[7] * ny + R[8] * nz;
        if (!front) {                      /* light the side being shown */
            vx = -vx; vy = -vy; vz = -vz;
        }
        float diff = vx * lx + vy * ly + vz * lz;
        if (diff < 0)
            diff = 0;
        float l = 0.35f + 0.65f * diff;
        int lit = lit_gamma[(int)(l * 128.0f + 0.5f)];
        int r = (m->clr[t * 3] * lit) >> 8;
        int gg = (m->clr[t * 3 + 1] * lit) >> 8;
        int bb = (m->clr[t * 3 + 2] * lit) >> 8;
        if (r > 255) r = 255;
        if (gg > 255) gg = 255;
        if (bb > 255) bb = 255;
        uint16_t c16 = SURF_RGB(r, gg, bb);
        uint32_t c32 = 0xff000000u | ((uint32_t)r << 16) |
                       ((uint32_t)gg << 8) | (uint32_t)bb;
        /* a textured face carries the lit baseColorFactor into the
         * pixel loop as multipliers over the texel it samples there */
        const surf_image *tx = (m->tmap && m->timg && m->tmap[t] >= 0)
                                   ? m->timg[m->tmap[t]] : NULL;
        const float *tp = tx ? &m->tuv[(size_t)t * 6] : NULL;

        float miny = p0[1] < p1[1] ? (p0[1] < p2[1] ? p0[1] : p2[1])
                                   : (p1[1] < p2[1] ? p1[1] : p2[1]);
        float maxy = p0[1] > p1[1] ? (p0[1] > p2[1] ? p0[1] : p2[1])
                                   : (p1[1] > p2[1] ? p1[1] : p2[1]);
        int y0i = (int)ceilf(miny - 0.5f);
        int y1i = (int)floorf(maxy - 0.5f);
        if (y0i < 0) y0i = 0;
        if (y1i > h - 1) y1i = h - 1;
        if (y1i < y0i)
            continue;

        /* per-edge inverse slopes once, then each row is mul-adds */
        const float *ev[3][2] = {{p0, p1}, {p1, p2}, {p2, p0}};
        float exy[3], edy[3], exd[3];      /* dx/dy, dd/dy per edge */
        float exu[3] = {0}, exv[3] = {0};  /* du/dy, dv/dy — textured only */
        const float *euv[3][2] = {{0}};
        if (tx) {
            euv[0][0] = tp;     euv[0][1] = tp + 2;
            euv[1][0] = tp + 2; euv[1][1] = tp + 4;
            euv[2][0] = tp + 4; euv[2][1] = tp;
        }
        for (int e = 0; e < 3; e++) {
            float dy = ev[e][1][1] - ev[e][0][1];
            float inv = fabsf(dy) > 1e-9f ? 1.0f / dy : 0.0f;
            exy[e] = (ev[e][1][0] - ev[e][0][0]) * inv;
            edy[e] = dy;
            exd[e] = (ev[e][1][2] - ev[e][0][2]) * inv;
            if (tx) {
                exu[e] = (euv[e][1][0] - euv[e][0][0]) * inv;
                exv[e] = (euv[e][1][1] - euv[e][0][1]) * inv;
            }
        }

        for (int iy = y0i; iy <= y1i; iy++) {
            float yc = (float)iy + 0.5f;
            float xl = 1e30f, xr = -1e30f, dl = 0, dr = 0;
            float ul = 0, vlq = 0, ur = 0, vrq = 0;
            for (int e = 0; e < 3; e++) {
                float ya = ev[e][0][1], yb = ev[e][1][1];
                if (edy[e] == 0.0f)
                    continue;
                if ((yc < ya && yc < yb) || (yc >= ya && yc >= yb))
                    continue;
                float x = ev[e][0][0] + (yc - ya) * exy[e];
                float dd = ev[e][0][2] + (yc - ya) * exd[e];
                float eu = 0, ew = 0;
                if (tx) {
                    eu = euv[e][0][0] + (yc - ya) * exu[e];
                    ew = euv[e][0][1] + (yc - ya) * exv[e];
                }
                if (x < xl) { xl = x; dl = dd; ul = eu; vlq = ew; }
                if (x > xr) { xr = x; dr = dd; ur = eu; vrq = ew; }
            }
            if (xr < xl)
                continue;
            int x0i = (int)ceilf(xl - 0.5f);
            int x1i = (int)floorf(xr - 0.5f);
            if (x0i < 0) x0i = 0;
            if (x1i > w - 1) x1i = w - 1;
            if (x1i < x0i)
                continue;
            float span = xr - xl;
            float sinv = span > 1e-9f ? 1.0f / span : 0.0f;
            float ddx = (dr - dl) * sinv;
            float pre = (float)x0i + 0.5f - xl;
            float dep = dl + pre * ddx;
            /* q in (0, 1/(CAM-1)]: scale into u16 with headroom */
            int32_t zf = (int32_t)(dep * 65536.0f * (MESH_CAM - 1.2f) * 0.9f);
            int32_t zs = (int32_t)(ddx * 65536.0f * (MESH_CAM - 1.2f) * 0.9f);
            uint16_t *zrow = ms.z + (size_t)iy * w;
            uint8_t *row = (uint8_t *)dst->pixels + (size_t)iy * dst->stride;
            if (tx) {
                /* affine UV across the row, wrapped tex_sample's way;
                 * the texel wears the face's lit baseColorFactor */
                float dudx = (ur - ul) * sinv, dvdx = (vrq - vlq) * sinv;
                float uu = ul + pre * dudx, vv = vlq + pre * dvdx;
                int tw = tx->w, th = tx->h;
                const uint8_t *tpx = tx->pixels;
                bool wide = dst->format != SURF_FMT_RGB565;
                for (int ix = x0i; ix <= x1i;
                     ix++, zf += zs, uu += dudx, vv += dvdx) {
                    uint16_t zi = (uint16_t)(zf < 0 ? 0
                                  : zf > 65535 ? 65535 : zf);
                    if (zi > zrow[ix]) {
                        zrow[ix] = zi;
                        float uw = uu - floorf(uu), vw = vv - floorf(vv);
                        int txx = (int)(uw * tw);
                        int tyy = (int)(vw * th);
                        if (txx >= tw) txx = tw - 1;
                        if (tyy >= th) tyy = th - 1;
                        uint32_t p = *(const uint32_t *)(tpx +
                                     (size_t)tyy * tx->stride +
                                     (size_t)txx * 4);
                        int pr = ((int)((p >> 16) & 0xff) * r) >> 8;
                        int pg = ((int)((p >> 8) & 0xff) * gg) >> 8;
                        int pb = ((int)(p & 0xff) * bb) >> 8;
                        if (wide)
                            ((uint32_t *)row)[ix] = 0xff000000u |
                                ((uint32_t)pr << 16) | ((uint32_t)pg << 8) |
                                (uint32_t)pb;
                        else
                            ((uint16_t *)row)[ix] = SURF_RGB(pr, pg, pb);
                    }
                }
            } else if (dst->format == SURF_FMT_RGB565) {
                uint16_t *px = (uint16_t *)row;
                for (int ix = x0i; ix <= x1i; ix++, zf += zs) {
                    uint16_t zi = (uint16_t)(zf < 0 ? 0
                                  : zf > 65535 ? 65535 : zf);
                    if (zi > zrow[ix]) {
                        zrow[ix] = zi;
                        px[ix] = c16;
                    }
                }
            } else {
                uint32_t *px = (uint32_t *)row;
                for (int ix = x0i; ix <= x1i; ix++, zf += zs) {
                    uint16_t zi = (uint16_t)(zf < 0 ? 0
                                  : zf > 65535 ? 65535 : zf);
                    if (zi > zrow[ix]) {
                        zrow[ix] = zi;
                        px[ix] = c32;
                    }
                }
            }
            drawn++;
        }
    }
    (void)drawn;
    return true;
}
