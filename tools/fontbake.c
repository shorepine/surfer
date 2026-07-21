/* Build-time font baker (DESIGN.md §2.5): rasterizes a TTF at one pixel
 * size into an A8 atlas + advance/kerning tables, emitted as a C header.
 * Runtime never rasterizes — text drawing is atlas blits.
 *
 *   fontbake NAME SIZE font.ttf out.h [ranges]
 *
 * ranges: comma-separated codepoint spans, e.g. "32-126,8230".
 * Default: ASCII 32-126 plus U+2026 (ellipsis, needed for ellipsize).
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define STB_TRUETYPE_IMPLEMENTATION
#include "stb/stb_truetype.h"

#define MAX_CPS 4096

static uint32_t cps[MAX_CPS];
static int ncps;

static void parse_ranges(const char *s)
{
    while (*s) {
        unsigned long a = strtoul(s, (char **)&s, 10), b = a;
        if (*s == '-')
            b = strtoul(s + 1, (char **)&s, 10);
        for (unsigned long c = a; c <= b && ncps < MAX_CPS; c++)
            cps[ncps++] = (uint32_t)c;
        if (*s == ',')
            s++;
    }
}

int main(int argc, char **argv)
{
    if (argc < 5) {
        fprintf(stderr, "usage: fontbake NAME SIZE font.ttf out.h [ranges]\n");
        return 1;
    }
    const char *name = argv[1];
    float size = (float)atof(argv[2]);
    parse_ranges(argc > 5 ? argv[5] : "32-126,8230");

    FILE *fp = fopen(argv[3], "rb");
    if (!fp) {
        fprintf(stderr, "fontbake: cannot open %s\n", argv[3]);
        return 1;
    }
    FILE *out = fopen(argv[4], "w");
    if (!out) {
        fprintf(stderr, "fontbake: cannot write %s\n", argv[4]);
        return 1;
    }
    fseek(fp, 0, SEEK_END);
    long fsz = ftell(fp);
    fseek(fp, 0, SEEK_SET);
    unsigned char *ttf = malloc((size_t)fsz);
    if (fread(ttf, 1, (size_t)fsz, fp) != (size_t)fsz)
        return 1;
    fclose(fp);

    stbtt_fontinfo info;
    if (!stbtt_InitFont(&info, ttf, stbtt_GetFontOffsetForIndex(ttf, 0))) {
        fprintf(stderr, "fontbake: bad font\n");
        return 1;
    }

    /* pack into the smallest power-of-two atlas that fits */
    stbtt_packedchar *pc = malloc(sizeof *pc * (size_t)ncps);
    int *cplist = malloc(sizeof(int) * (size_t)ncps);
    for (int i = 0; i < ncps; i++)
        cplist[i] = (int)cps[i];
    unsigned char *atlas = NULL;
    int aw = 128, ah = 128;
    for (;;) {
        atlas = realloc(atlas, (size_t)aw * ah);
        stbtt_pack_context pctx;
        stbtt_PackBegin(&pctx, atlas, aw, ah, aw, 1, NULL);
        stbtt_pack_range range = {
            .font_size = size,
            .array_of_unicode_codepoints = cplist,
            .num_chars = ncps,
            .chardata_for_range = pc,
        };
        int ok = stbtt_PackFontRanges(&pctx, ttf, 0, &range, 1);
        stbtt_PackEnd(&pctx);
        if (ok)
            break;
        if (ah < aw) ah *= 2; else aw *= 2;   /* grow atlas, retry */
        if (aw > 4096) {
            fprintf(stderr, "fontbake: atlas won't fit\n");
            return 1;
        }
    }

    /* FONTBAKE_THRESHOLD=1 -> 1-bit atlas (no antialiasing): crisp
     * bitmap-font look, every pixel fully on or off.
     *
     * FONTBAKE_THRESHOLD_CUT=N moves the coverage cut (default 128).
     * It matters at small sizes: stb_truetype does not hint, so a stem
     * narrower than a pixel can land at ~40% coverage in every cell it
     * crosses and disappear entirely at the halfway cut, shredding the
     * font. Lowering the cut fattens strokes back to solid. Anything
     * below ~64 starts closing counters in 'e' and 'a'. */
    if (getenv("FONTBAKE_THRESHOLD")) {
        const char *c = getenv("FONTBAKE_THRESHOLD_CUT");
        int cut = c ? atoi(c) : 128;
        if (cut < 1) cut = 1;
        if (cut > 255) cut = 255;
        for (int j = 0; j < aw * ah; j++)
            atlas[j] = atlas[j] >= cut ? 255 : 0;
    }

    float scale = stbtt_ScaleForPixelHeight(&info, size);
    int ascent, descent, gap;
    stbtt_GetFontVMetrics(&info, &ascent, &descent, &gap);
    int ascent_px  = (int)(ascent * scale + 0.5f);
    int descent_px = (int)(descent * scale - 0.5f);
    int gap_px     = (int)(gap * scale + 0.5f);

    /* collect the non-zero kern pairs once (used by both emitters) */
    uint32_t *ka = malloc(sizeof(uint32_t) * (size_t)ncps * ncps);
    uint32_t *kb = malloc(sizeof(uint32_t) * (size_t)ncps * ncps);
    int16_t  *kd = malloc(sizeof(int16_t) * (size_t)ncps * ncps);
    int nkern = 0;
    for (int i = 0; i < ncps; i++)
        for (int j = 0; j < ncps; j++) {
            int k = stbtt_GetCodepointKernAdvance(&info, (int)cps[i], (int)cps[j]);
            int px = (int)(k * scale + (k < 0 ? -0.5f : 0.5f));
            if (px) { ka[nkern] = cps[i]; kb[nkern] = cps[j]; kd[nkern] = (int16_t)px; nkern++; }
        }

    int m_idx = -1;
    for (int i = 0; i < ncps; i++) if (cps[i] == 'M') m_idx = i;
    int m_adv = m_idx >= 0 ? (int)(pc[m_idx].xadvance + 0.5f) : 0;
    int line_h = ascent_px - descent_px + gap_px;

    const char *op = argv[4];
    int is_py = strlen(op) >= 3 && strcmp(op + strlen(op) - 3, ".py") == 0;

    if (is_py) {
        /* runtime-loadable blob (surf_font_from_blob): little-endian
         *   "SFN1", u16 w, u16 h, i16 asc, i16 desc, i16 gap, i16 0,
         *   u32 nglyphs, u32 nkerns, then atlas[w*h],
         *   glyphs[nglyphs]{u32 cp, i16 x,y,w,h,xoff,yoff,adv},
         *   kerns[nkerns]{u32 a, u32 b, i16 adv} */
        size_t sz = 24 + (size_t)aw * ah + (size_t)ncps * 18 + (size_t)nkern * 10;
        uint8_t *blob = malloc(sz), *w = blob;
        #define PU16(v) do { *w++ = (uint8_t)(v); *w++ = (uint8_t)((v) >> 8); } while (0)
        #define PU32(v) do { PU16((v) & 0xffff); PU16(((uint32_t)(v)) >> 16); } while (0)
        memcpy(w, "SFN1", 4); w += 4;
        PU16(aw); PU16(ah);
        PU16((uint16_t)ascent_px); PU16((uint16_t)descent_px);
        PU16((uint16_t)gap_px); PU16(0);
        PU32((uint32_t)ncps); PU32((uint32_t)nkern);
        memcpy(w, atlas, (size_t)aw * ah); w += (size_t)aw * ah;
        for (int i = 0; i < ncps; i++) {
            const stbtt_packedchar *g = &pc[i];
            PU32(cps[i]);
            PU16((uint16_t)g->x0); PU16((uint16_t)g->y0);
            PU16((uint16_t)(g->x1 - g->x0)); PU16((uint16_t)(g->y1 - g->y0));
            PU16((uint16_t)(int)(g->xoff + (g->xoff < 0 ? -0.5f : 0.5f)));
            PU16((uint16_t)(int)(g->yoff + (g->yoff < 0 ? -0.5f : 0.5f)));
            PU16((uint16_t)(int)(g->xadvance + 0.5f));
        }
        for (int i = 0; i < nkern; i++) { PU32(ka[i]); PU32(kb[i]); PU16((uint16_t)kd[i]); }

        fprintf(out, "# Generated by tools/fontbake.c — do not edit. %s %.1fpx, cell %dx%d\n",
                name, size, m_adv, line_h);
        fprintf(out, "FONT = (");
        for (size_t i = 0; i < sz; i += 16) {
            fprintf(out, "\n    b'");
            for (size_t j = i; j < i + 16 && j < sz; j++)
                fprintf(out, "\\x%02x", blob[j]);
            fprintf(out, "'");
        }
        fprintf(out, "\n)\n");
        fprintf(stderr, "fontbake: %s %.1fpx -> blob %zu bytes, cell %dx%d\n",
                name, size, sz, m_adv, line_h);
        return 0;
    }

    fprintf(out, "/* Generated by tools/fontbake.c — do not edit. %s %.1fpx from %s */\n",
           name, size, argv[3]);
    fprintf(out, "#include \"surfer.h\"\n\n");

    fprintf(out, "static const uint8_t surf_font_%s_px[%d] = {\n", name, aw * ah);
    for (int i = 0; i < aw * ah; i += 24) {
        fprintf(out, "    ");
        for (int j = i; j < i + 24 && j < aw * ah; j++)
            fprintf(out, "%d,", atlas[j]);
        fprintf(out, "\n");
    }
    fprintf(out, "};\n\n");

    fprintf(out, "static const surf_glyph surf_font_%s_glyphs[%d] = {\n", name, ncps);
    for (int i = 0; i < ncps; i++) {
        const stbtt_packedchar *g = &pc[i];
        fprintf(out, "    {%u, %d, %d, %d, %d, %d, %d, %d},\n", cps[i],
               g->x0, g->y0, g->x1 - g->x0, g->y1 - g->y0,
               (int)(g->xoff + (g->xoff < 0 ? -0.5f : 0.5f)),
               (int)(g->yoff + (g->yoff < 0 ? -0.5f : 0.5f)),
               (int)(g->xadvance + 0.5f));
    }
    fprintf(out, "};\n\n");

    fprintf(out, "static const surf_kern surf_font_%s_kerns[] = {\n", name);
    for (int i = 0; i < nkern; i++)
        fprintf(out, "    {%u, %u, %d},\n", ka[i], kb[i], kd[i]);
    fprintf(out, "    {0, 0, 0},\n};\n\n");  /* keep the array non-empty */

    fprintf(out, "static const surf_font surf_font_%s = {\n", name);
    fprintf(out, "    .atlas = {.pixels = (void *)surf_font_%s_px, .w = %d, .h = %d,\n"
           "              .stride = %d, .format = SURF_FMT_A8, .opaque = false},\n",
           name, aw, ah, aw);
    fprintf(out, "    .ascent = %d, .descent = %d, .line_gap = %d,\n",
           ascent_px, descent_px, gap_px);
    fprintf(out, "    .glyphs = surf_font_%s_glyphs, .nglyphs = %d,\n", name, ncps);
    fprintf(out, "    .kerns = surf_font_%s_kerns, .nkerns = %d,\n", name, nkern);
    fprintf(out, "};\n");

    fprintf(stderr, "fontbake: %s %.1fpx -> %dx%d atlas, %d glyphs, cell %dx%d (M adv x line_h)\n",
            name, size, aw, ah, ncps, m_adv, line_h);
    return 0;
}
