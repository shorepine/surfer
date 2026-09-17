/* surfer — public API. This header is the whole binding surface; keep it
 * flat and boring (see DESIGN.md §3). */
#ifndef SURFER_H
#define SURFER_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct { int16_t x, y, w, h; } surf_rect;
typedef struct { int16_t x, y; } surf_point;

/* RGB565 everywhere the framebuffer is concerned (DESIGN.md §5.1). */
typedef uint16_t surf_color;
#define SURF_RGB(r, g, b) \
    ((surf_color)((((r) & 0xf8) << 8) | (((g) & 0xfc) << 3) | (((b) & 0xf8) >> 3)))

typedef enum {
    SURF_FMT_RGB565   = 0,
    SURF_FMT_ARGB8888 = 1,
    SURF_FMT_A8       = 2,  /* alpha-only (glyph atlases); tinted on blend */
    /* STORAGE ONLY, never a live image: a fully-solid glyph atlas packed
     * one bit per pixel, MSB first, rows padded to a byte. fontbake emits
     * it for any bake with no partial coverage (a BDF, or a pixel outline
     * at its exact ppem) and surf_image_expand_a1() unpacks it to A8 the
     * first time the font is looked up. Nothing below the font registry
     * ever sees it — surf_image_new() refuses it, the hal has no bytes-
     * per-pixel for it, and the PPA has no A1 blend mode. It exists
     * because seven eighths of such an atlas is padding, and on the
     * device the app partition is what runs out. */
    SURF_FMT_A1       = 3,
} surf_format;

typedef struct {
    void      *pixels;
    int16_t    w, h;
    int32_t    stride;  /* bytes per row; must be a 64-byte multiple on device */
    uint8_t    format;  /* surf_format */
    bool       opaque;  /* no alpha content: compositor may use blit, not blend */
    surf_color tint;    /* A8 only: the color the alpha modulates */
} surf_image;

/* Runtime images (sprites loaded after boot, any size). Pixels come from
 * hal->alloc_image, so surf_init must have run. PNG decodes to ARGB8888
 * (opaque flag set when the file has no transparency). Destroy only
 * after every node using the image is gone. */
surf_image *surf_image_from_png(const void *data, size_t len);

/* ...and back out again: any image this library holds, encoded as a PNG
 * in memory. Free what it returns with surf_image_png_free.
 *
 * An image can be drawn into (the shape API, or a caller writing its own
 * pixels through the MicroPython buffer) and until this there was no way
 * to save one, so anything that made a picture could show it and never
 * keep it. A8 encodes as white-with-that-alpha, since an A8 image is a
 * mask whose colour lives in the node's tint. */
void *surf_image_to_png(const surf_image *img, size_t *len);
void  surf_image_png_free(void *png);
/* The PNG's alpha channel as an A8 mask: draws in the image's `tint`
 * color — a one-entry palette the P4 blends in hardware. Retint + damage
 * the sprite each frame for Amiga-style color cycling. */
surf_image *surf_image_from_png_a8(const void *data, size_t len);
void        surf_image_destroy(surf_image *img);

/* Load-time image composition: bake tile maps / parallax strips into one
 * image so the frame path pays one blit per LAYER, not one per tile.
 * These run on the CPU and must never be called per frame.
 * surf_image_new: SURF_FMT_RGB565 = opaque (starts black),
 * SURF_FMT_ARGB8888 starts fully transparent. blit alpha-composites
 * src over dst (565/ARGB/A8 sources). */
surf_image *surf_image_new(int16_t w, int16_t h, surf_format format);

/* ...and the same thing for an image something DRAWS INTO EVERY FRAME.
 *
 * surf_image_new hands out whatever memory a backend has most of, which
 * is the right default: images are usually baked once and then only
 * read, and there are a lot of them. An emulator's screen, a video
 * decoder's surface or a software renderer's target is the other case --
 * written pixel by pixel, sixty times a second -- and there the distance
 * to the memory is the cost.
 *
 * Measured on the P4X, one 256x224 RGB565 frame of a NES emulator: the
 * palette blit filling it cost 2.44 ms into PSRAM and 1.45 ms into
 * internal SRAM. The other 0.5 ms is not the blit at all -- pushing
 * 114 KB through the cache each frame evicts the emulator's own working
 * set, so the 6502 slowed down too.
 *
 * FALLS BACK SILENTLY. Fast memory is scarce and a big image will not
 * fit; you get an ordinary image and the old speed, never a failure. */
surf_image *surf_image_new_fast(int16_t w, int16_t h, surf_format format);

/* Scale the whole of `src` into the whole of `dst`, bilinear.
 *
 * Uses the backend's 2D engine when it has one (the P4's PPA) and a CPU
 * loop otherwise, so it always works and is sometimes free. Returns true
 * if the hardware did it, which is worth knowing when you are deciding
 * how much of it to do per frame.
 *
 * DOWN THEN UP IS A BLUR, and a wide one for the price of two scaled
 * blits rather than a kernel per pixel -- the cheapest blur available on
 * hardware like this by a long way. Formats must match. */
bool surf_image_scale(surf_image *dst, const surf_image *src);

/* Publish CPU writes to img->pixels so the drawing path can read them.
 *
 * Call it after writing an image's pixels YOURSELF — through the `pixels`
 * pointer, or through the MicroPython Image buffer — and before the node
 * showing that image is damaged. It is a no-op on backends whose blitter
 * reads the same memory the CPU wrote (SDL, web) and a cache writeback where
 * the blitter is a DMA engine (the P4's PPA), so the portable rule is simply
 * to call it: a caller that gets it right on a laptop and wrong on the panel
 * is the exact bug this exists to prevent.
 *
 * surf_image_fill/blit/poly and friends do NOT call it for you -- they are
 * often used in runs, and one flush after the last of them beats one per
 * call. */
void surf_image_flush(const surf_image *img);

void surf_image_fill(surf_image *dst, surf_rect r, surf_color c);
void surf_image_blit(surf_image *dst, const surf_image *src, surf_rect src_r,
                     int16_t x, int16_t y);
/* blit rotated by quarter turns CCW (bake rotated props — a fallen tree
 * is a standing one at rot 1 — so the frame path stays untransformed) */
void surf_image_blit_rot(surf_image *dst, const surf_image *src,
                         surf_rect src_r, int16_t x, int16_t y, uint8_t rot);

/* Load-time vector shapes (src/core/shape.c): anti-aliased fills and
 * round-capped strokes baked into an image — the frame path never sees
 * a curve. Coords are Q16 pixels. Drawing into an SURF_FMT_A8 image
 * puts coverage in alpha: the whole drawing recolors by .tint (cycling
 * shapes for the price of a blend). Never call these per frame. */
enum { SURF_PAINT_SOLID = 0, SURF_PAINT_LINEAR = 1 };
typedef struct {
    uint8_t    kind;            /* SURF_PAINT_* */
    surf_color c0, c1;          /* endpoint colors (solid uses c0/a0) */
    uint8_t    a0, a1;          /* endpoint alphas */
    int32_t    x0, y0, x1, y1;  /* gradient axis, Q16 dst coords */
} surf_paint;
void surf_image_poly(surf_image *dst, const int32_t *xy_q16, int n,
                     const surf_paint *paint);          /* filled, nonzero */
void surf_image_polyline(surf_image *dst, const int32_t *xy_q16, int n,
                         int32_t width_q16, const surf_paint *paint);
void surf_image_ellipse(surf_image *dst, int32_t cx_q16, int32_t cy_q16,
                        int32_t rx_q16, int32_t ry_q16,
                        int32_t width_q16 /* 0 = fill */,
                        const surf_paint *paint);
void surf_image_bezier(surf_image *dst, const int32_t xy_q16[8],
                       int32_t width_q16, const surf_paint *paint);  /* cubic */

/* ---- 3D: low-poly glTF meshes, software-rendered into an image ----
 *
 * surf_mesh_from_glb loads a .glb (glTF 2 binary): mode-4 triangles,
 * float positions, node transforms flattened, and by default ONE flat
 * color per triangle — material baseColorFactor times either the
 * baseColorTexture sampled at the face's UV centroid (exact for
 * palette-textured low-poly art) or the COLOR_0 centroid. The texture
 * is read at load and freed; a texture the file references by URI
 * (Kenney keeps one colormap.png beside its models) comes from
 * tex_png/tex_len, NULL for none. The model is centered and normalized
 * so render's size_px is simply its radius in pixels. On failure
 * returns NULL with the reason in err.
 *
 * SURF_MESH_TEXTURED in flags keeps the decoded textures RESIDENT and
 * samples them per PIXEL at render (nearest texel, affine UV) instead
 * of once per face at load — for art whose detail lives inside a face
 * (a painted model, not a palette), where the centroid sample collapses
 * every face to one color. It costs the texture's memory for the
 * mesh's lifetime and a sampler in the inner loop; a face with no
 * texture still renders flat, so the flag is safe on any model.
 *
 * surf_mesh_render is the software-renderer case surf_image_flush
 * documents: a z-buffered flat-shaded rasterize into an RGB565 or
 * ARGB8888 image, on the CALLER's event — never the compose path. Like
 * fill/blit/poly it does NOT flush; flush once after the last write and
 * before damaging the node. Rotations are degrees (ry spins, rx tilts,
 * rz rolls); cx/cy place the model's center in dst pixels. cull: -1
 * honors each material's doubleSided flag, 1 always culls back faces
 * (cheaper, right for closed models), 0 draws both sides. Render SMALL
 * and let the sprite's .scale do the enlarging — that is the PPA's SRM
 * on the device, and free. */
typedef struct surf_mesh surf_mesh;
#define SURF_MESH_TEXTURED 1u   /* keep textures, sample per pixel */
surf_mesh *surf_mesh_from_glb(const void *glb, size_t len,
                              const void *tex_png, size_t tex_len,
                              unsigned flags, char *err, size_t errcap);
void surf_mesh_destroy(surf_mesh *m);
int  surf_mesh_tris(const surf_mesh *m);
bool surf_mesh_render(const surf_mesh *m, surf_image *dst,
                      float rx_deg, float ry_deg, float rz_deg,
                      float size_px, float cx, float cy, int cull);

typedef enum {
    SURF_TOUCH_DOWN = 0,
    SURF_TOUCH_MOVE = 1,
    SURF_TOUCH_UP   = 2,
} surf_touch_phase;

/* `id` is the CONTACT this event belongs to — a controller track id,
 * stable from DOWN to UP. It is last in the struct on purpose: every
 * positional `(surf_touch){x, y, phase}` in the tree keeps compiling and
 * gets contact 0, which is exactly right for a mouse. */
typedef struct { int16_t x, y; uint8_t phase; uint8_t id; } surf_touch;
/* How many fingers dispatch can follow at once. The GT911 panel reports
 * five; a hal with more would simply have its extra contacts ignored. */
#define SURF_MAX_CONTACTS 5
/* one multitouch contact (surf_hal.touch_points / surf_touch_points);
 * id is the controller's track id, stable while the finger is down */
typedef struct { int16_t x, y; uint8_t id; } surf_touch_pt;

/* The hal vtable — the only thing a backend implements (DESIGN.md §2.1). */
typedef struct {
    void (*fill)(surf_rect dst, surf_color c);
    void (*blit)(const surf_image *src, surf_rect src_r, surf_point dst);
    void (*blend)(const surf_image *src, surf_rect src_r, surf_point dst, uint8_t opa);
    /* Draw src_r scaled into dst_r, mirrored (bit0 = x, bit1 = y, applied
     * to the source before rotation) and rotated by `rot` quarter turns
     * CCW (matching the P4 PPA's SRM engine), alpha-blended unless the
     * image is opaque AND opa is 255; only the `vis` sub-rect of dst_r
     * must be written. dst_r is the post-rotation footprint.
     *
     * `opa` scales the whole draw the way it does in `blend` above — the
     * two are one op with and without a transform, so a parameter on one
     * and not the other made node opacity work on an untransformed
     * sprite and silently do nothing the moment somebody scaled it. */
    void (*xform_blend)(const surf_image *src, surf_rect src_r, surf_rect dst_r,
                        surf_rect vis, uint8_t rot, uint8_t mirror, uint8_t opa);
    void (*present)(const surf_rect *dirty, int n);
    void (*wait_idle)(void);
    uint64_t (*now_us)(void);
    bool (*poll_touch)(surf_touch *out);
    /* Optional (may be NULL): current multitouch contacts, id-stable
     * while a finger stays down. Poll-style — apps that need more than
     * the single-pointer dispatch (piano keyboards, XY pads) read this
     * each frame and diff by id; everything else keeps using on_touch.
     * Returns the number of contacts written (<= max). */
    int (*touch_points)(surf_touch_pt *out, int max);
    void *(*alloc_image)(size_t bytes);  /* 64-byte aligned */
    /* Optional (may be NULL): image memory the CPU writes FAST, for the
     * rare image that is rendered into per frame rather than baked once.
     * On a backend with a memory hierarchy that means the near side of
     * it -- internal SRAM on the P4, where alloc_image hands out PSRAM.
     *
     * It is a hint and it may fail: this memory is scarce, so returning
     * NULL is normal and the caller falls back to alloc_image. Whatever
     * comes back must still be drawable BY THE BLITTER, which on a DMA
     * backend is the constraint that decides whether this can exist at
     * all. free_image takes either. */
    void *(*alloc_image_fast)(size_t bytes);
    void (*free_image)(void *p);
    /* Optional (may be NULL): scale one whole image into another, on
     * hardware. Returns true if it did; false means "not for these two"
     * and the caller falls back to the CPU.
     *
     * This is the one image-to-image operation worth a hal hook, because
     * on a backend with a 2D engine it is FREE and on the CPU it is not:
     * a downscale followed by an upscale IS a blur (it is what dual-
     * Kawase bloom is built from), so a blur, a bloom and a
     * depth-of-field all come from the same op. */
    bool (*scale_image)(surf_image *dst, const surf_image *src);
    /* Optional (may be NULL): make CPU writes to an image's pixels visible
     * to whatever reads them for drawing — a cache writeback on a backend
     * whose blitter is a DMA engine, nothing at all where the compositor
     * reads the same memory the CPU wrote.
     *
     * The hal's own note says "CPU never touches the compose buffer, so the
     * only cache sync in the system is after asset uploads", and that held
     * while every image was written once and then only read. It stops
     * holding the moment something renders INTO an image every frame — a
     * scope trace, a video decoder, an emulator — which is what
     * surf_image_flush() and the MicroPython Image buffer exist for. */
    void (*sync_image)(const void *buf, size_t bytes);
    /* Optional (may be NULL): CPU pointer to the current RGB565 compose
     * target. Exists for exactly one caller — the textgrid fast path —
     * because the PPA's ~85µs-per-op floor makes per-glyph blits ~15×
     * too slow for full-screen text (see DESIGN.md §5.6). The hal owns
     * any cache sync of CPU-written regions at present time. */
    void *(*fb_ptr)(int32_t *stride_bytes);
    /* Optional (may be NULL): shift the pixels inside r vertically by
     * dy (>0 = content moves up); the vacated dy rows are left for the
     * caller to repaint. The hal owns cache and multi-buffer coherence.
     * Exists for textgrid fast scrolling — measured on the P4, a full
     * page of CPU-rendered text costs 46 ms but a DMA shift + one-row
     * repaint fits a 60 fps budget (DESIGN.md §5.6). */
    void (*scroll_rect)(surf_rect r, int16_t dy);
    /* Streaming band shift for scrolling layers: move the band's content
     * by (sx, sy) relative to the LAST PRESENTED frame. Contract: the
     * caller shifts EVERY frame while streaming — (0, 0) on sub-pixel
     * frames, which must refresh the band unchanged (a crawling layer
     * must not pay a repaint per pixel; measured as fps tracking scroll
     * speed) — damages only the exposed slivers, and repaints the whole
     * band once when streaming ends. In exchange the backend may skip
     * its usual write-back bookkeeping for the band (on the P4 this is
     * one cross-buffer DMA2D copy and no damage-forward, the difference
     * between 19 and 60 fps). Optional; NULL means layers always
     * repaint. */
    void (*band_shift)(surf_rect r, int16_t sx, int16_t sy);
    /* Optional frame lock (game mode): block until `divisor` panel
     * refreshes have passed since the last locked frame — 60/divisor
     * fps on the P4's 60 Hz panel. Early frames wait; late frames slip
     * whole periods (quantized cadence, no catch-up bursts). NULL when
     * the backend has no vsync to lock to. */
    void (*wait_frame)(int divisor);
    /* Optional: the panel's measured refresh rate in Hz (the divisor
     * base for wait_frame). NULL → assume 60. */
    float (*frame_hz)(void);
} surf_hal;

typedef struct surf_node surf_node;

/* Widget values are Q16 fixed point in [0, SURF_ONE]. */
#define SURF_ONE 65536

typedef void (*surf_touch_cb)(surf_node *n, const surf_touch *t, void *user);
typedef void (*surf_change_cb)(int32_t value_q16, void *user);
typedef void (*surf_index_cb)(int32_t index, void *user);

typedef struct {
    int        max_nodes;  /* node pool size; 0 → 256 */
    surf_color bg;         /* fill color under non-opaque content */
} surf_config;

/* lifecycle */
bool       surf_init(const surf_hal *hal, int16_t w, int16_t h, const surf_config *cfg);
void       surf_deinit(void);
surf_node *surf_screen(void);
void       surf_tick(void);  /* compose dirty rects + present */
/* current multitouch contacts (0 when the hal has no multi support) */
int        surf_touch_points(surf_touch_pt *out, int max);
/* Game mode: lock surf_tick to panel_rate/divisor fps (see hal
 * wait_frame). 0 (default) = uncapped. Steady 30 beats a 45-60 wobble:
 * pick the divisor your worst frame always fits. */
void       surf_set_frame_divisor(int divisor);
float      surf_frame_hz(void);  /* panel refresh rate (60 if unknown) */

/* ---- input feed (src/core/keys.c) ----------------------------------
 * Abstract input from any source. A DRIVER (USB HID, SDL keyboard, i2c
 * gamepad, ...) converts its hardware into these calls; surfer core
 * queues/stores them and the MicroPython module exposes them. No
 * hardware knowledge lives in surfer — a driver says "key A pressed" or
 * (via surf_pad_*) "button 2 down, axis 1 = -0.2", not "USB endpoint
 * 0x81 reported...". */
typedef enum {
    SURFER_KEY_TEXT = 0,   /* utf8[] holds the typed text */
    SURFER_KEY_LEFT, SURFER_KEY_RIGHT, SURFER_KEY_UP, SURFER_KEY_DOWN,
    SURFER_KEY_PGUP, SURFER_KEY_PGDN, SURFER_KEY_HOME, SURFER_KEY_END,
    SURFER_KEY_BACKSPACE, SURFER_KEY_DELETE, SURFER_KEY_ENTER,
    /* Esc is a KEY, not text. It used to close the desktop window from
     * inside the SDL pump -- convenient for a C demo, catastrophic for a
     * host: on tulip5 one Esc took down the machine, every running app
     * and anything unsaved, from a key people press to mean "cancel
     * what I just started". A host that wants it to quit can do that
     * itself now, and one that wants to CANCEL something can have it. */
    SURFER_KEY_ESC,
} surfer_key_kind;
/* MODIFIERS ARE FLAGS, NOT SPELLINGS, and ctrl is a flag because it
 * could not be a spelling. ctrl+LETTER has a control character to become
 * (^S is 0x13) and a driver hands that over as TEXT, which is what a
 * terminal puts on the wire and what every consumer already understands.
 * ctrl+DELETE, ctrl+arrow, ctrl+Home have no such character — so before
 * this flag a driver had one option, drop the modifier, and both of
 * surfer's drivers did exactly that ("ctrl+arrow still arrows"). Every
 * chord on a non-letter was therefore indistinguishable from the bare
 * key: an editor could not bind delete-word, a list could not bind
 * jump-to-end, and nothing downstream could tell it was missing.
 *
 * The other way out was ^Tab's: invent a private code and send it as
 * text. That is right for ONE chord that means one thing (0x1e = switch
 * app) and wrong as a general answer — a ctrl+Left delivered as a
 * private character stops being a LEFT, so a widget switching on `kind`
 * no longer sees an arrow at all, and every consumer needs the table.
 * A flag leaves the key what it is and lets whoever cares look. */
typedef struct { uint8_t kind; bool shift; bool ctrl; char utf8[8]; } surfer_key;

void surf_key_event(const surfer_key *k);   /* push a discrete key (typing/repeat) */
void surf_key_set_held(const surfer_key *keys, int n);  /* replace the held set */
bool surf_key_poll(surfer_key *out);        /* drain one queued event (module) */
int  surf_key_held(surfer_key *out, int max);  /* current held set (module) */
void surf_key_reset(void);                  /* clear queue + held */

/* ---- controllers (src/core/pad.c) ----------------------------------
 * A normalized game pad: an 8-way dpad/hat, face + shoulder buttons,
 * and two analog sticks. SOURCES write it (USB gamepad, i2c stick,
 * the built-in keyboard map, an on-screen touch pad); GAMES read it.
 * The whole point of the layer is that a game maps abstract pad state
 * to actions and never learns whether the input arrived over USB, i2c,
 * a keyboard, or a finger. Slots are for local multiplayer. Axes are
 * Q16 in [-SURF_ONE, SURF_ONE]; a source with only a dpad leaves them
 * zero, and a source with only a stick leaves the dpad zero — a game
 * can read whichever it prefers (or both). */
#define SURF_MAX_PADS 4
enum {   /* face + shoulder buttons (bitmask) */
    SURF_BTN_A     = 1u << 0, SURF_BTN_B      = 1u << 1,
    SURF_BTN_X     = 1u << 2, SURF_BTN_Y      = 1u << 3,
    SURF_BTN_L     = 1u << 4, SURF_BTN_R      = 1u << 5,
    SURF_BTN_START = 1u << 6, SURF_BTN_SELECT = 1u << 7,
};
enum {   /* dpad / hat (bitmask; a diagonal is two bits set = 8-way) */
    SURF_DPAD_UP   = 1u << 0, SURF_DPAD_DOWN  = 1u << 1,
    SURF_DPAD_LEFT = 1u << 2, SURF_DPAD_RIGHT = 1u << 3,
};
/* read (games) — safe on any index; out-of-range returns neutral */
uint8_t  surf_pad_dpad(int pad);
uint16_t surf_pad_buttons(int pad);
int32_t  surf_pad_axis(int pad, int stick, int axis);   /* 0=x 1=y */
/* write (sources) — each call replaces that field; axes clamp to range.
 * Digital controls have TWO source channels that reads OR together, so
 * a gamepad (source 0) and the keyboard map (source 1) can both drive
 * one pad and either works. set_dpad/set_buttons target source 0; the
 * _src variants pick the channel. */
void surf_pad_set_dpad(int pad, uint8_t dpad);
void surf_pad_set_buttons(int pad, uint16_t buttons);
void surf_pad_set_dpad_src(int pad, int src, uint8_t dpad);
void surf_pad_set_buttons_src(int pad, int src, uint16_t buttons);
void surf_pad_set_axis(int pad, int stick, int axis, int32_t val_q16);
void surf_pad_reset(int pad);       /* neutral (a source disconnected) */
void surf_pad_reset_all(void);

/* Host chrome drawn OUTSIDE the machine's screen (tulip5's iOS key
 * bar). chrome_q16 is the fraction of the window height it wants kept
 * clear at the bottom, Q16 — a FRACTION because the host measures in
 * its own space and SDL's window height need not agree; ctrl_latch is a
 * one-shot Ctrl from such chrome, consumed by the next character typed
 * — the only way a soft keyboard with no ctrl key can send ^C. Weak
 * zeroes in input.c, so a host that draws none needs to know nothing
 * about either. */
extern int surf_host_chrome_q16;
/* ...and the same at the TOP: a notch, an island, a status bar. Kept
 * clear rather than paid for with slack — a caller can shrink what it
 * asks for and let the centring push the picture down, but only while
 * there IS slack, and there is none when a screen keyboard has taken
 * the bottom half of the window. */
extern int surf_host_chrome_top_q16;
extern int surf_host_ctrl_latch;

/* the platform screen keyboard (iOS): op -1 asks, 1 summons, 0
 * dismisses; returns what is actually shown, or -1 where the platform
 * has none — a desktop, a board with real keys. Weak default in
 * input.c; the SDL hal overrides it. */
int surf_screen_keyboard(int op);

/* node constructors (from the pool; NULL when exhausted) */
surf_node *surf_group_new(int16_t x, int16_t y);
surf_node *surf_rect_new(int16_t x, int16_t y, int16_t w, int16_t h, surf_color c);
surf_node *surf_sprite_new(const surf_image *img, int16_t x, int16_t y);
/* Layer: a horizontally wrap-scrolling strip (parallax backgrounds, tile
 * maps baked with surf_image_new/blit). view_w is the on-screen width;
 * the strip wraps at strip->w. Offset is Q16 pixels; fast scroll (needs
 * an opaque strip + hal band_shift) turns per-frame motion into one DMA
 * band copy plus a sliver repaint. Overlay nodes drawn on top of a fast
 * layer must be LATER SIBLINGS in the same parent — the layer damages
 * them as it shifts. Fast layers must not overlap each other. */
surf_node *surf_layer_new(const surf_image *strip, int16_t x, int16_t y,
                          int16_t view_w);
void       surf_layer_set_offset(surf_node *n, int32_t off_q16);
int32_t    surf_layer_offset(const surf_node *n);
void       surf_layer_set_fast_scroll(surf_node *n, bool on);
surf_node *surf_filmstrip_new(const surf_image *img, int16_t frame_w, int16_t frame_h,
                              int16_t x, int16_t y);
surf_node *surf_ninepatch_new(const surf_image *img, int16_t x, int16_t y,
                              int16_t w, int16_t h,
                              int16_t l, int16_t t, int16_t r, int16_t b);

/* scrollview: a clipped group whose children draw at a content offset.
 * Dragging inside it scrolls after an 8px threshold steals the gesture
 * from child handlers (widgets that own drags — sliders, knobs — opt out
 * via surf_node_set_gesture_grab). Momentum and edge spring-back run in
 * surf_tick, in core, so every backend feels identical (DESIGN.md §2.6). */
surf_node *surf_scrollview_new(int16_t x, int16_t y, int16_t w, int16_t h);
void       surf_scrollview_set_offset(surf_node *sv, int16_t x, int16_t y);
surf_point surf_scrollview_offset(const surf_node *sv);
surf_point surf_scrollview_content_size(surf_node *sv);
/* Fast scroll (opt-in, same contract as the textgrid's): vertical scroll
 * shifts the viewport pixels via the hal and repaints only the exposed
 * strip instead of every visible child. The caller promises the viewport
 * is fully on-screen and unoccluded. Ignored without hal scroll_rect. */
void surf_scrollview_set_fast_scroll(surf_node *sv, bool on);

/* tree — detach keeps the subtree fully alive (DESIGN.md §2.2) */
void surf_node_add(surf_node *parent, surf_node *child);
void surf_node_detach(surf_node *child);
void surf_node_destroy(surf_node *n);  /* detaches, then frees the subtree */

/* A node's slot in the pool, or -1 if it is not a pool node. Stable for
 * the node's lifetime and reused after it, which is exactly what a
 * binding wants to key a side table of wrapper objects on. */
int surf_node_index(const surf_node *n);

/* Called for EVERY node the library frees — including the children
 * destroy() takes with their parent, which a caller has no other way to
 * learn about. A language binding holds one wrapper object per node and
 * uses this to drop it; without it the wrapper both leaks and is left
 * pointing at a pool slot that has already been handed to someone else.
 * Not called for the free-list build in surf_init. */
typedef void (*surf_node_freed_fn)(surf_node *n, int index);
void surf_set_node_freed_cb(surf_node_freed_fn cb);

/* properties — every write damages the old and new screen rects */
void surf_node_set_pos(surf_node *n, int16_t x, int16_t y);
void surf_node_damage(surf_node *n);   /* force a repaint (e.g. after retint) */
/* Do two nodes overlap ON THE INK? Boxes first (AABB on absolute
 * positions and w/h — transformed sprites use their transformed
 * footprint), and where the boxes touch, a sprite or filmstrip then
 * answers from its image's alpha: pixels under SURF_INK_ALPHA (128) do
 * not collide, so a sword's transparent corners no longer bounce a
 * ball. Everything else — rects, groups, labels, and any image with no
 * transparency — is still its whole box. False if either is hidden or
 * detached. The collision primitive for games: the box reject keeps
 * every-bullet-vs-every-enemy cheap, and the per-pixel walk only runs
 * over the intersection of two boxes that already touch. */
bool surf_node_overlaps(const surf_node *a, const surf_node *b);
void surf_node_set_hidden(surf_node *n, bool hidden);

/* OPACITY — one alpha multiplier over everything a node draws.
 *
 * 255 is the default and costs nothing: the compositor takes exactly the
 * paths it always did. Below that the node's draw becomes a blend scaled
 * by `opa`, which on the P4 is the PPA blend unit's own
 * fg_alpha_scale_ratio and on SDL is a multiply already in the loop — so
 * this is a parameter the hal had all along and nothing above ever set.
 *
 * WHICH NODES, and the line is not arbitrary: a node that draws a
 * PICTURE, i.e. everything painted through hal->blend. Sprite,
 * filmstrip, ninepatch, layer and label. A rect, a textgrid and a
 * textinput's chrome are hal->fill, which has no opacity — a blended
 * fill is a hal op that does not exist, and inventing one is a bigger
 * change than this. surf_node_can_fade() is the question; setting it on
 * anything else is REFUSED (false) rather than quietly ignored, and the
 * MicroPython binding raises. That is the .rot-on-a-group lesson: a
 * silent no-op on the wrong node type costs somebody an afternoon.
 *
 * A GROUP IS REFUSED, and permanently rather than pending work. Fading a
 * group means compositing it whole and then blending the result once,
 * which needs an offscreen render target; blending each child separately
 * is a different picture — two overlapping children both show through
 * each other, and the overlap comes out wrong. surfer composites
 * straight into the framebuffer by construction (DESIGN.md §1), so the
 * honest answer is to fade the children.
 *
 * IT IS VISUAL ONLY. Hit testing, surf_node_overlaps and the ink test
 * ignore it, so a fade cannot change what a finger or a collision does
 * halfway through. `hidden` is still the way to take a node out of both.
 *
 * WHAT IT COSTS: a translucent node is not opaque, so it stops covering
 * what is behind it — the compositor's occlusion early-out no longer
 * stops at it, and everything underneath is painted too. It also turns
 * OFF band_shift streaming for a fast-panning sprite or layer (a shifted
 * band holds an already-composited result; re-blending it would blend
 * twice). Both come back the moment it returns to 255. Fading a
 * full-screen backdrop is therefore a real bill; fading a 64px sprite is
 * not. */
bool surf_node_can_fade(const surf_node *n);
bool surf_node_set_opacity(surf_node *n, uint8_t opa);  /* false: wrong type */
uint8_t surf_node_opacity(const surf_node *n);          /* 255 where unsupported */

/* ...AND OVER TIME, along with everything else a node can be moved by.
 *
 * `surf_node_tween(n, prop, to, ms, ease)` walks ONE property to a
 * value. surf_tick drives it, so the app writes one line and never
 * holds a clock, and it keeps running through frames the app itself is
 * not being called for — a screen backgrounded mid-move comes back
 * finished rather than frozen half way. Same machinery as a playing
 * filmstrip, ticked beside it.
 *
 * SEVERAL AT ONCE ON ONE NODE, because the table is keyed by (node,
 * property): a dying enemy drifts up and fades, which is two tweens.
 * Starting a second on the SAME property replaces it, which is what
 * reversing a fade mid-flight means.
 *
 * `to` IS Q16 FOR EVERY PROPERTY — SURF_TW_OPACITY 0..255<<16, X and Y
 * in pixels<<16, SCALE the same Q16 the transform already uses. One
 * type through the interpolation; only the write knows the difference.
 *
 * A DIRECT WRITE TO THAT PROPERTY CANCELS ITS TWEEN — set_pos kills an
 * X or Y tween, set_xform kills a SCALE one, set_opacity kills a fade —
 * otherwise the tween would overwrite it on the next tick and the write
 * would look like it did nothing. Destroying a node cancels all of its
 * tweens; ms <= 0 lands on the end value at once.
 *
 * THERE IS NO ROT TWEEN, and that is the hardware rather than an
 * omission: the P4's PPA rotates in QUARTER TURNS only (surf_sprite_set_xform
 * refuses anything else), so a "smooth" rotate could only ever be a
 * four-frame flip-book. Asking for one is refused rather than served
 * badly. */
enum {
    SURF_TW_OPACITY = 0,
    SURF_TW_X       = 1,
    SURF_TW_Y       = 2,
    SURF_TW_SCALE   = 3,
    SURF_TW_NPROPS  = 4,
};
/* Linear is the default and is right for a fade. MOTION is what wants
 * the others: ease-out on a slide is the difference between cheap and
 * polished, which is why these arrived with position tweens and not
 * with opacity. */
enum {
    SURF_EASE_LINEAR  = 0,
    SURF_EASE_IN      = 1,   /* quadratic */
    SURF_EASE_OUT     = 2,
    SURF_EASE_IN_OUT  = 3,
};
bool surf_node_tween(surf_node *n, uint8_t prop, int32_t to_q16,
                     int32_t ms, uint8_t ease);
/* prop < 0 cancels every tween on the node */
void surf_node_tween_cancel(surf_node *n, int prop);
bool surf_node_tweening(const surf_node *n, int prop);   /* prop < 0 = any */
int32_t surf_node_tween_value(const surf_node *n, uint8_t prop);  /* Q16 now */

/* Opacity's named shortcuts, which is what a fade reads as. */
bool surf_node_fade_to(surf_node *n, uint8_t to, int32_t ms);
void surf_node_fade_cancel(surf_node *n);
bool surf_node_fading(const surf_node *n);
void surf_rect_set_color(surf_node *n, surf_color c);
void surf_rect_set_size(surf_node *n, int16_t w, int16_t h);
void surf_sprite_set_src(surf_node *n, surf_rect src);
/* SWAP THE PICTURE. A sprite's image used to be fixed at birth, so a
 * slime that died meant destroying its node and building another in its
 * place — and the cheap answer (both states in one image, set_src
 * picking a cell) only covers pictures somebody baked together. This
 * covers the rest: two files, two sizes, an image rendered at runtime.
 *
 * The src window RESETS to the whole new picture, which is what the
 * constructor would have given, and the node's footprint follows —
 * scale, rotation, mirror, hitboxes, fast_pan and the filmstrip's
 * transport all stay, because they are the NODE's and not the
 * picture's. Both old and new rects are damaged. Swapping in the image
 * already there is a no-op and does NOT reset a src window.
 *
 * On a FILMSTRIP the declared frame size stays and the frame COUNT is
 * recomputed from the new strip (the frame clamps into it), so trading
 * a walk cycle for a death cycle at the same cel size is one call.
 * False means nothing changed: no node, not a sprite or filmstrip, no
 * image, or a strip too small to hold one frame. */
bool surf_sprite_set_image(surf_node *n, const surf_image *img);
/* Fast pan (opt-in): when only src.x/src.y change on an identity,
 * opaque, unclipped, fully-on-screen sprite — a camera window over a
 * big baked world image — the move becomes one hal band_shift plus
 * sliver repaints. Same contract as fast layers: later siblings
 * overlaying the sprite get damaged as it pans; the sprite repaints
 * fully once when panning stops (call set_src every frame). */
void surf_sprite_set_fast_pan(surf_node *n, bool on);
/* Uniform scale (Q16; SURF_ONE = 1:1, PPA range ~1/16..16), rotation in
 * quarter turns CCW (0..3 — the P4's SRM engine only does 90° steps),
 * and mirror (bit0 = x flip, bit1 = y flip; applied to the source before
 * rotation). The node's w/h become the transformed footprint. */
void    surf_sprite_set_xform(surf_node *n, int32_t scale_q16, uint8_t rot,
                              uint8_t mirror);
int32_t surf_sprite_scale(const surf_node *n);
uint8_t surf_sprite_rot(const surf_node *n);
uint8_t surf_sprite_mirror(const surf_node *n);

/* HITBOXES: caller-authored collision rects on a sprite or filmstrip.
 * Offsets are in the UNROTATED source frame's coordinates -- negative
 * or past w/h is legal (a reach that sticks out of the picture) -- and
 * collision maps them through the node's transform with the same
 * arithmetic the ink mask uses, so a box authored on the head stays on
 * the head through every mirror and quarter turn. While a node has any,
 * they REPLACE its box-and-ink answer in surf_node_overlaps: the boxes
 * are what collides, and the OTHER node's ink still counts. At most
 * SURF_MAX_HITBOXES (32) per node, so a hit reports as a bitmask. */
typedef struct { int16_t x, y, w, h; } surf_hitbox;
#define SURF_MAX_HITBOXES 32
int32_t surf_hitbox_add(surf_node *n, int16_t x, int16_t y,
                        int16_t w, int16_t h);      /* -> index, or -1 */
bool    surf_hitbox_set(surf_node *n, int32_t i, int16_t x, int16_t y,
                        int16_t w, int16_t h);
bool    surf_hitbox_get(const surf_node *n, int32_t i, surf_hitbox *out);
bool    surf_hitbox_remove(surf_node *n, int32_t i); /* later boxes shift down */
int32_t surf_hitbox_count(const surf_node *n);
/* the box mapped through the transform, in ABSOLUTE screen coords --
 * what actually collides, and what a debug outline should draw */
bool    surf_hitbox_abs(const surf_node *n, int32_t i, surf_rect *out);
/* which of a's hitboxes overlap b: bit i set = hitbox i hit */
uint32_t surf_node_overlaps_which(const surf_node *a, const surf_node *b);
/* the node's parent, for a caller placing chrome beside it */
surf_node *surf_node_parent(surf_node *n);
/* Can this node carry a transform at all? Only sprites and filmstrips
 * can; a binding that lets a caller write `rot` to anything else should
 * refuse rather than let surf_sprite_set_xform quietly drop it. */
bool surf_node_can_xform(const surf_node *n);
void surf_group_set_clip(surf_node *g, int16_t w, int16_t h);  /* 0×0 disables */
void surf_filmstrip_set_frame(surf_node *n, int16_t frame);
/* Play it: frames per second in Q16, 0 (the default) to leave the frame
 * to the caller. A playing strip advances from surf_tick and wraps;
 * late frames are dropped rather than replayed. */
void    surf_filmstrip_set_fps(surf_node *n, int32_t fps_q16);
int32_t surf_filmstrip_fps(const surf_node *n);
/* The transport, a second axis on purpose: set_playing(false) freezes
 * the strip KEEPING its fps, so set_playing(true) resumes at the speed
 * it had — which zeroing fps cannot do. playing() answers "is it
 * actually advancing": fps set AND not stopped. */
void    surf_filmstrip_set_playing(surf_node *n, bool on);
bool    surf_filmstrip_playing(const surf_node *n);
void    surf_filmstrip_tick(void);
int16_t surf_filmstrip_frame(const surf_node *n);
void surf_ninepatch_set_size(surf_node *n, int16_t w, int16_t h);

/* input: touch routes to the hit node's nearest ancestor with a handler,
 * which holds pointer capture until UP (DESIGN.md §2.6) */
surf_node *surf_hit_test(int16_t x, int16_t y);
/* A wheel / two-finger push at (x, y), in pixels of content movement.
 * Scrolls the first scrollable scrollview under the pointer — the hal
 * calls this; nothing else needs to.
 *
 * WHAT NO SCROLLVIEW TAKES IS QUEUED for the application, and drained
 * with surf_wheel_poll (surfer.wheel() in Python). That is the same
 * bargain touch already makes — a widget under the pointer wins, and
 * what nothing claimed is the app's to interpret — and it is what lets
 * a wheel mean something other than scrolling: zooming a picture,
 * stepping a value, spinning a knob. Coordinates are FRAMEBUFFER
 * pixels, mapped by the hal exactly as a touch is. */
void surf_input_wheel(int16_t x, int16_t y, int16_t dx, int16_t dy);
typedef struct { int16_t x, y, dx, dy; } surf_wheel;
bool surf_wheel_poll(surf_wheel *out);      /* drain one unclaimed event */
void surf_wheel_reset(void);                /* drop the queue (new session) */
void surf_node_set_on_touch(surf_node *n, surf_touch_cb cb, void *user);
void surf_node_abs_pos(const surf_node *n, int16_t *x, int16_t *y);
surf_point surf_node_pos(const surf_node *n);
surf_point surf_node_size(const surf_node *n);
/* decode the first codepoint of a UTF-8 string (0 for empty/NULL) */
uint32_t surf_utf8_first(const char *s);
/* grab = an enclosing scrollview may not steal this node's gestures */
void surf_node_set_gesture_grab(surf_node *n, bool grab);
/* feed a synthetic touch through the normal dispatch path (tests, OSK) */
void surf_inject_touch(const surf_touch *t);

/* ---- text: atlases baked at build time by tools/fontbake.c ---- */

typedef struct {
    uint32_t cp;
    int16_t  x, y, w, h;   /* atlas rect */
    int16_t  xoff, yoff;   /* bearing from the pen position */
    int16_t  adv;
} surf_glyph;

typedef struct { uint32_t a, b; int16_t adv; } surf_kern;

typedef struct surf_font {
    surf_image        atlas;   /* SURF_FMT_A8, or ARGB8888 for an emoji set */
    int16_t           ascent, descent, line_gap;  /* px; descent ≤ 0 */
    const surf_glyph *glyphs;  /* sorted by codepoint */
    int32_t           nglyphs;
    const surf_kern  *kerns;   /* sorted by (a, b); only non-zero pairs */
    int32_t           nkerns;
    /* Tried for a codepoint this face has not got, BEFORE giving up and
     * drawing '?'. It is how emoji work: every text face points at the
     * emoji set nearest its own size, so a label or a cell holding
     * U+1F525 draws fire instead of a question mark, and nothing above
     * the font layer had to learn a second way to draw a picture.
     *
     * One level, and deliberately: a chain is a lookup whose cost
     * depends on how many faces are loaded, and the answer to "what if
     * the fallback also misses" is that it draws '?' like anything else.
     * The registry wires this at load; nothing else may, since a font
     * pointing at itself would not return. */
    const struct surf_font *fallback;
} surf_font;

#define surf_font_line_h(f) ((int16_t)((f)->ascent - (f)->descent + (f)->line_gap))

/* Load a fontbake blob (the "SFN1" bytes it emits to a .py module) into
 * a runtime font — pass it to surf_textgrid_new for a custom console
 * font. Decoded at load time; free with surf_font_free after every
 * textgrid using it is gone. */
surf_font *surf_font_from_blob(const void *data, size_t len);
void       surf_font_free(surf_font *f);

/* True if every glyph has the same advance — the textgrid needs that,
 * since its cell is sized from 'M' and wider glyphs would be clipped.
 * An emoji face is mono by construction (every picture is one box wide)
 * and is skipped here anyway: it is a FALLBACK, never a grid's own face. */
bool surf_font_is_mono(const surf_font *f);

/* True if this face draws pictures rather than shapes — an ARGB atlas,
 * so the caller's colour does not apply to it. The textgrid asks,
 * because its cell composer has a separate loop for each. */
bool surf_font_is_color(const surf_font *f);

/* ---- emoji ----
 * The set baked from assets/emoji/set.txt, reachable by name so a caller
 * does not have to write `"\U0001F525"` and hope. Returns 0 for a name
 * that is not in the set — never a placeholder codepoint, since a
 * silently wrong picture is worse than a visible nothing. */
uint32_t    surf_emoji_cp(const char *name);
int         surf_emoji_count(void);
const char *surf_emoji_name_at(int idx);   /* NULL past the end */
uint32_t    surf_emoji_cp_at(int idx);

/* ---- built-in fonts ----
 * Every font baked into this build, addressable by name ("ui16",
 * "helvR12", "bigblue12", ...). Names are the fontbake output names.
 * Lookups return a shared, prepared instance — do not free it. */
const surf_font *surf_font_builtin(const char *name);
int              surf_font_builtin_count(void);
const char      *surf_font_builtin_name(int idx);
const surf_font *surf_font_builtin_at(int idx);

/* Backends whose blitter can't read the atlas where the linker put it
 * (the P4's PPA can't DMA from memory-mapped flash) call this once at
 * startup to re-home every built-in atlas; later lookups see the
 * prepared copy. */
/* Unpack a SURF_FMT_A1 atlas into a freshly allocated A8 one, in place.
 * No-op (true) for any other format. False only if the allocation failed,
 * in which case the image is left drawable-but-empty rather than
 * half-converted. Load time only — never the frame path. */
bool surf_image_expand_a1(surf_image *img);

typedef void (*surf_font_prepare_fn)(surf_image *atlas);
void surf_font_builtin_prepare(surf_font_prepare_fn fn);

typedef enum {
    SURF_ALIGN_LEFT   = 0,
    SURF_ALIGN_CENTER = 1,
    SURF_ALIGN_RIGHT  = 2,
} surf_align;

surf_point surf_text_measure(const surf_font *f, const char *str, int16_t wrap_w);

/* A string rendered into a freshly allocated ARGB image — glyphs,
 * colour, wrap and the fallback face exactly as a label draws them.
 * For the one thing a label cannot do: SCALE. Text nodes have no
 * transform (a per-glyph blit cannot ride the sprite path), so big
 * chunky text is this image on a SPRITE with .scale — bake at the
 * face's own size and let the compositor do the enlarging. One-shot,
 * not frame path; the caller owns the image. NULL on no font, no
 * string, or no memory. */
surf_image *surf_text_bake(const surf_font *f, const char *str,
                           surf_color color, int16_t wrap_w);

/* label node: wrap at wrap_w (0 = single line), greedy break on space and
 * hyphen; ellipsize truncates a single line with U+2026 instead */
surf_node *surf_text_new(const surf_font *f, const char *str,
                         int16_t x, int16_t y, surf_color c);
void surf_text_set(surf_node *n, const char *str);
void surf_text_set_color(surf_node *n, surf_color c);
void surf_text_set_wrap(surf_node *n, int16_t wrap_w);
void surf_text_set_align(surf_node *n, surf_align a);
void surf_text_set_ellipsis(surf_node *n, bool on);

/* textinput node: single-line editable text + caret/selection state.
 * Indices are byte offsets into the UTF-8 buffer, always on a codepoint
 * boundary. The box/border art and the on-screen keyboard are widgets
 * built on top, not core (DESIGN.md §2.5). */
surf_node  *surf_textinput_new(const surf_font *f, int16_t x, int16_t y,
                               int16_t w, surf_color c);
void        surf_textinput_set_text(surf_node *n, const char *str);
const char *surf_textinput_text(const surf_node *n);
void        surf_textinput_insert(surf_node *n, const char *utf8);  /* at caret */
void        surf_textinput_backspace(surf_node *n);
void        surf_textinput_delete(surf_node *n);
int32_t     surf_textinput_caret(const surf_node *n);
void        surf_textinput_set_caret(surf_node *n, int32_t byte_idx, bool extend);
void        surf_textinput_move(surf_node *n, int32_t delta_cp, bool extend);
int32_t     surf_textinput_index_from_x(const surf_node *n, int16_t local_x);
void        surf_textinput_set_focused(surf_node *n, bool focused);
/* Password fields: draw `c` for every character (0 = show the text). The
 * buffer is untouched — surf_textinput_text() still returns what was
 * typed — and the caret, the hit test and the paint all measure the mask,
 * so the caret lands where the asterisks are. */
/* ...and the MULTILINE form: the same node, laid out with the wrap the
 * text node already has and a height in LINES. A textarea is a
 * textinput with more than one line — sharing the node keeps the
 * buffer, the caret, the selection and every edit one implementation
 * rather than two. `rows` in lines of the face it is given, because a
 * face is the host's preference and a field asked for as five lines has
 * to be five lines of whatever that turns out to be. */
surf_node  *surf_textarea_new(const surf_font *f, int16_t x, int16_t y,
                              int16_t w, int16_t rows, surf_color c);
int16_t     surf_textinput_rows(const surf_node *n);   /* 0 = single line */
int16_t     surf_textinput_lines(const surf_node *n);  /* wrapped, of text */
void        surf_textinput_scroll_to(surf_node *n, int16_t y);
int16_t     surf_textinput_scroll_y(const surf_node *n);
int32_t     surf_textinput_index_from_xy(const surf_node *n, int16_t local_x,
                                         int16_t local_y);
void        surf_textinput_move_line(surf_node *n, int dir, bool extend);
void        surf_textinput_set_mask(surf_node *n, char c);
char        surf_textinput_mask(const surf_node *n);

/* LED: an indicator lamp — the one widget that reports nothing, because a
 * lamp is an output. The art is A8, so one asset is any color: each LED
 * keeps its own copy of the image struct (shared pixels, its own tint),
 * which on the P4 is a palette register the PPA applies at blend time.
 * Brightness is a level, not a bool, so a blink can fade. */
typedef struct surf_led surf_led;

typedef struct {
    const surf_image *strip;   /* A8 filmstrip, dark..lit */
    int16_t           frame_w, frame_h, frames;
    surf_color        color;
} surf_led_style;

surf_led  *surf_led_new(surf_node *parent, int16_t x, int16_t y,
                        const surf_led_style *style);
void       surf_led_destroy(surf_led *l);
surf_node *surf_led_node(surf_led *l);
void       surf_led_set(surf_led *l, bool on);
void       surf_led_set_level(surf_led *l, int32_t level_q16);
int32_t    surf_led_level(const surf_led *l);
void       surf_led_set_color(surf_led *l, surf_color c);

/* Colour picker: a saturation/value square beside a hue strip, reporting
 * a packed surf_color. The one widget whose art cannot be baked — the
 * square's colours depend on the hue — so it draws into two runtime
 * images per pixel, on an EVENT: the strip once at creation, the square
 * again only when the hue changes. Never in the frame path. */
typedef struct surf_colorpicker surf_colorpicker;

surf_colorpicker *surf_colorpicker_new(surf_node *parent, int16_t x, int16_t y,
                                       int16_t size);
void       surf_colorpicker_destroy(surf_colorpicker *c);
surf_node *surf_colorpicker_node(surf_colorpicker *c);
surf_color surf_colorpicker_color(const surf_colorpicker *c);
void       surf_colorpicker_set_color(surf_colorpicker *c, surf_color col);
void       surf_colorpicker_on_change(surf_colorpicker *c, surf_index_cb cb,
                                      void *user);

/* Scrollbar: a thumb on a track, driven by a content model the CALLER
 * owns. It knows nothing about what is scrolling — hand it total, visible
 * and pos in any unit (console rows, editor lines, panel pixels) and it
 * reports a new pos when dragged. Hides itself while total <= visible, so
 * a caller can set the range unconditionally. */
typedef struct surf_scrollbar surf_scrollbar;

typedef struct {
    const surf_image *thumb;   /* 9-patch capsule; its w is the thickness */
    const surf_image *track;   /* optional, same art fainter */
    int16_t           inset;   /* 9-patch inset along the axis */
    /* A 9-patch slices along fixed axes, so a horizontal bar needs the
     * capsule LYING DOWN — stretching the vertical art sideways tiles
     * its round cap into beads. Optional: without them a horizontal bar
     * falls back to the vertical art and looks wrong. */
    const surf_image *thumb_h;
    const surf_image *track_h;
} surf_scrollbar_style;

surf_scrollbar *surf_scrollbar_new(surf_node *parent, int16_t x, int16_t y,
                                   int16_t len, bool vertical,
                                   const surf_scrollbar_style *style);
void      surf_scrollbar_destroy(surf_scrollbar *s);
surf_node *surf_scrollbar_node(surf_scrollbar *s);
void      surf_scrollbar_set_range(surf_scrollbar *s, int32_t total,
                                   int32_t visible, int32_t pos);
void      surf_scrollbar_set_pos(surf_scrollbar *s, int32_t pos);
int32_t   surf_scrollbar_pos(const surf_scrollbar *s);
void      surf_scrollbar_on_change(surf_scrollbar *s, surf_change_cb cb,
                                   void *user);

/* textgrid: the fast fixed-width text mode (terminals, code editors).
 * A cols×rows grid of opaque cells (codepoint + fg + bg), composed by
 * the CPU straight into the framebuffer — full-screen text scrolls at
 * frame rate where per-glyph blits cannot (DESIGN.md §5.6). Requires a
 * monospaced font; the cell is sized from 'M'. */
surf_node *surf_textgrid_new(const surf_font *f, int16_t cols, int16_t rows,
                             surf_color fg, surf_color bg);
void surf_textgrid_set_cell(surf_node *n, int16_t col, int16_t row, uint32_t cp,
                            surf_color fg, surf_color bg);
/* fill a row from UTF-8 in the default colors, space-padded to the edge */
void surf_textgrid_set_row(surf_node *n, int16_t row, const char *utf8);
/* positive = content moves up; exposed rows are blanked */
void surf_textgrid_scroll(surf_node *n, int16_t dy_rows);
surf_point surf_textgrid_cell_size(const surf_node *n);
/* Recolour a live grid — a console changing its background while you
 * watch. Cells still holding the old defaults follow; anything a caller
 * coloured by hand keeps what it was given. */
void surf_textgrid_set_colors(surf_node *n, surf_color fg, surf_color bg);
/* Fast scroll (opt-in): scroll() shifts the framebuffer pixels via the
 * hal and repaints only the exposed rows, instead of re-rendering every
 * cell. The caller promises the grid is fully visible and unoccluded on
 * screen (the terminal / code-editor case). Ignored when the hal has no
 * scroll_rect. */
void surf_textgrid_set_fast_scroll(surf_node *n, bool on);

/* Scrollback (opt-in): keep mult x the visible rows, so lines that scroll
 * off the top stay reachable. Drag the grid to look back — a thin bar
 * appears on the right while there is history — and any write snaps the
 * view to the bottom again, the way a terminal does. Costs
 * cols*rows*mult*sizeof(surf_textcell); a 128x50 console at 10x is
 * ~500 KB, which is why it is not the default. Enabling it installs the
 * grid's own touch handler, so do not also set on_touch on that node. */
bool    surf_textgrid_set_scrollback(surf_node *n, int16_t mult);
int16_t surf_textgrid_history(const surf_node *n);  /* rows above the view */
int16_t surf_textgrid_view(const surf_node *n);     /* rows scrolled back */
void    surf_textgrid_set_view(surf_node *n, int16_t back);

typedef struct {
    const surf_image *strip;  /* 2 frames: unchecked, checked */
    int16_t           frame_w, frame_h;
} surf_checkbox_style;

typedef struct surf_checkbox surf_checkbox;

surf_checkbox *surf_checkbox_new(surf_node *parent, int16_t x, int16_t y,
                                 const surf_checkbox_style *style);
void       surf_checkbox_destroy(surf_checkbox *c);
surf_node *surf_checkbox_node(surf_checkbox *c);
bool       surf_checkbox_checked(const surf_checkbox *c);
void       surf_checkbox_set_checked(surf_checkbox *c, bool on);  /* no cb */
void       surf_checkbox_on_change(surf_checkbox *c, surf_change_cb cb, void *user);

typedef struct {
    const surf_image *normal;  /* 9-patch, unpressed */
    const surf_image *pressed;
    int16_t           inset;
    const surf_font  *font;
    surf_color        text_color;
} surf_button_style;

typedef struct surf_button surf_button;

surf_button *surf_button_new(surf_node *parent, int16_t x, int16_t y,
                             int16_t w, int16_t h, const surf_button_style *style,
                             const char *label);
void       surf_button_destroy(surf_button *b);
surf_node *surf_button_node(surf_button *b);
void       surf_button_set_label(surf_button *b, const char *label);
/* fires on release inside the button (value is always SURF_ONE) */
void       surf_button_on_press(surf_button *b, surf_change_cb cb, void *user);

typedef struct {
    const surf_image *panel;   /* 9-patch for the box and the popup */
    int16_t           inset;
    const surf_font  *font;
    surf_color        text_color, hi_color;
    const surf_image *arrow;   /* 2-frame strip: closed, open; NULL = none */
    int16_t           arrow_w, arrow_h;
} surf_dropdown_style;

typedef struct surf_dropdown surf_dropdown;

/* The open popup attaches to the screen root so it overlays siblings —
 * detach/reattach in action. Item strings are copied. */
surf_dropdown *surf_dropdown_new(surf_node *parent, int16_t x, int16_t y, int16_t w,
                                 const surf_dropdown_style *style,
                                 const char *const *items, int32_t nitems);
void       surf_dropdown_destroy(surf_dropdown *d);
surf_node *surf_dropdown_node(surf_dropdown *d);
int32_t    surf_dropdown_selected(const surf_dropdown *d);
void       surf_dropdown_set_selected(surf_dropdown *d, int32_t index);  /* no cb */
void       surf_dropdown_on_change(surf_dropdown *d, surf_index_cb cb, void *user);

/* ---- widgets: built from nodes, styled by caller-owned assets ---- */

/* The track keeps the art's OWN cross-axis size, centred, and stretches
 * only along its length — a 9-patch tiled across the groove repeats it,
 * and two parallel grooves is what that looks like. So a track narrower
 * than the widget is a legitimate shape (the compact slider is a thin bar
 * with a wider handle riding on it), and the widget clips its group to
 * the size it was asked for so the whole box stays grabbable. */
typedef struct {
    const surf_image *track;  /* 9-patch source */
    int16_t           inset;  /* uniform 9-slice inset */
    const surf_image *cap;    /* cap sprite */
    /* the same two lying down, for a slider wider than it is tall. A
     * 9-patch slices along fixed axes, so the upright art cannot be
     * stretched sideways — see the scrollbar. Optional: without them a
     * horizontal slider still works and still looks wrong. */
    const surf_image *track_h, *cap_h;
    /* the CAP's colour: it is A8, so one asset is any colour and a
     * retint costs a repaint and no pixels. 0 means the house grey. */
    surf_color        color;
} surf_slider_style;

typedef struct surf_slider surf_slider;

surf_slider *surf_slider_new(surf_node *parent, int16_t x, int16_t y,
                             int16_t w, int16_t h, const surf_slider_style *style);
void       surf_slider_destroy(surf_slider *s);
surf_node *surf_slider_node(surf_slider *s);  /* root group: detach/reattach */
/* The CAP's colour — the handle is the A8 part; the track keeps its own
 * art. Same bargain as the knob: one asset, any colour, no pixels. */
void       surf_slider_set_color(surf_slider *s, surf_color c);
surf_color surf_slider_color(const surf_slider *s);
void       surf_slider_set_value(surf_slider *s, int32_t value_q16);  /* no cb */
int32_t    surf_slider_value(const surf_slider *s);
void       surf_slider_on_change(surf_slider *s, surf_change_cb cb, void *user);

typedef struct {
    const surf_image *strip;  /* A8 filmstrip: frames left-to-right */
    int16_t           frame_w, frame_h;
    int16_t           frames;
    /* The colour the art's alpha is drawn in. The strip is A8 — a
     * silhouette, not a picture — so ONE asset is any colour: each knob
     * keeps its own copy of the surf_image (shared pixels, its own tint)
     * exactly as the LED does, and set_color costs a repaint and no
     * pixels. 0 means the house grey. */
    surf_color        color;
} surf_knob_style;

/* ---- built-in knob art, baked on first use (src/widgets/art.c) ----
 *
 * The 64-frame A8 filmstrips the knob and the selector draw from, at any
 * face size, rendered by the CPU into a hal-allocated image the first
 * time a size is asked for and shared by every widget of that size from
 * then on. They used to ship as 565 KB of flash .rodata; the faces are
 * tools/gen_widget_assets.py's, ported. NULL if the image could not be
 * allocated. Never freed. */
#define SURF_ART_FRAMES 64
const surf_image *surf_art_knob_strip(int16_t size);
const surf_image *surf_art_selector_strip(int16_t size);

typedef enum {
    SURF_KNOB_DRAG_VERTICAL = 0,  /* DAW convention (DESIGN.md §2.6) */
    SURF_KNOB_DRAG_ANGULAR  = 1,
} surf_knob_mode;

typedef struct surf_knob surf_knob;

surf_knob *surf_knob_new(surf_node *parent, int16_t x, int16_t y,
                         const surf_knob_style *style);
void       surf_knob_destroy(surf_knob *k);
surf_node *surf_knob_node(surf_knob *k);
/* Retint. A repaint, not a reblit — see the note on surf_knob_style. */
void       surf_knob_set_color(surf_knob *k, surf_color c);
surf_color surf_knob_color(const surf_knob *k);
void       surf_knob_set_mode(surf_knob *k, surf_knob_mode mode);
void       surf_knob_set_value(surf_knob *k, int32_t value_q16);  /* no cb */
int32_t    surf_knob_value(const surf_knob *k);
void       surf_knob_on_change(surf_knob *k, surf_change_cb cb, void *user);
/* A TAP on the knob — a press that travelled under a few px, so not a
 * drag. The value is unchanged; this reports that the user pointed AT
 * this knob, which a caller turns into a settings popup. The reported
 * value is WHERE in the knob's height it landed (Q16 0..SURF_ONE, top to
 * bottom), so "the top third is special" is the caller's policy. */
void       surf_knob_on_tap(surf_knob *k, surf_change_cb cb, void *user);

/* Selector: a knob with DETENTS. It chooses among N fixed options and
 * reports an index — a mode switch, a bank, a waveform — sharing the
 * knob's filmstrip so N stays a runtime number. Drag snaps as it goes;
 * a tap advances one position and wraps, which is how a 4-position
 * switch gets nudged on a touchscreen. */
/* Tabs: a strip of labelled buttons with a PAGE group behind each, and
 * the widget owns which page is showing. The art is the button's, so a
 * tab matches the rest of the chrome and bakes nothing new; the current
 * tab wears the pressed face.
 *
 * `surf_tabs_page(t, i)` is the point of it: a group to fill, that
 * nothing else has to know about. Hiding is not the caller's job, which
 * matters because `hidden` cannot be read back off a node — a caller
 * doing this by hand keeps its own shadow of what is showing. */
typedef struct surf_tabs surf_tabs;

/* The tab's own art: A8, rounded at the TOP and flat at the foot, so the
 * current tab joins the page under it. Both faces come from ONE image
 * with two tints — `face` is meant to be the PAGE's background, which is
 * what makes the join invisible, and `dim` is everything else. */
typedef struct {
    const surf_image *patch;   /* A8 9-patch: rounded top, square bottom */
    int16_t           inset_side, inset_top, inset_bottom;
    const surf_font  *font;
    const surf_font  *font_active;   /* NULL = the same face */
    surf_color        face, dim;         /* current tab, the others */
    surf_color        text_active, text;
} surf_tabs_style;

/* h is the WHOLE height, tab strip included; pages get h - tab_h. */
surf_tabs *surf_tabs_new(surf_node *parent, int16_t x, int16_t y,
                         int16_t w, int16_t h, int16_t tab_h,
                         const surf_tabs_style *style,
                         const char *const *labels, int32_t count);
void       surf_tabs_destroy(surf_tabs *t);
surf_node *surf_tabs_node(surf_tabs *t);
surf_node *surf_tabs_page(surf_tabs *t, int32_t i);
int32_t    surf_tabs_index(const surf_tabs *t);
int32_t    surf_tabs_count(const surf_tabs *t);
void       surf_tabs_set_index(surf_tabs *t, int32_t idx);  /* no cb */
void       surf_tabs_set_label(surf_tabs *t, int32_t i, const char *label);
void       surf_tabs_set_face(surf_tabs *t, surf_color c);  /* the current tab */
void       surf_tabs_set_dim(surf_tabs *t, surf_color c);   /* all the others */
/* ...and per tab, for a strip whose pages each carry their own paper:
 * tab i's bright face (that page's background) and its dim one (a
 * darker shade of the same, so an unselected tab still says which
 * page it opens). */
void       surf_tabs_set_face_at(surf_tabs *t, int32_t i, surf_color c);
void       surf_tabs_set_dim_at(surf_tabs *t, int32_t i, surf_color c);
void       surf_tabs_on_change(surf_tabs *t, surf_index_cb cb, void *user);

/* Radio: N options, exactly one chosen, in a column or a row. The
 * checkbox's sibling — a checkbox answers yes/no about itself, a radio
 * answers "which one" for a group, and the art says so. Both axes,
 * because a settings panel wants a column and a strip wants a row. */
typedef struct {
    const surf_image *strip;     /* 2 frames: empty ring, ring + dot */
    int16_t           frame_w, frame_h;
    const surf_font  *font;
    surf_color        text_color;
    int16_t           gap;       /* between options; 8 if 0 */
} surf_radio_style;

typedef struct surf_radio surf_radio;

surf_radio *surf_radio_new(surf_node *parent, int16_t x, int16_t y,
                           const surf_radio_style *style,
                           const char *const *labels, int32_t count,
                           bool vertical);
void       surf_radio_destroy(surf_radio *r);
surf_node *surf_radio_node(surf_radio *r);
int32_t    surf_radio_index(const surf_radio *r);
int32_t    surf_radio_count(const surf_radio *r);
surf_point surf_radio_size(const surf_radio *r);   /* as measured */
void       surf_radio_set_index(surf_radio *r, int32_t idx);  /* no cb */
void       surf_radio_on_change(surf_radio *r, surf_index_cb cb, void *user);

typedef struct surf_selector surf_selector;

surf_selector *surf_selector_new(surf_node *parent, int16_t x, int16_t y,
                                 const surf_knob_style *style,
                                 int32_t positions);
void       surf_selector_destroy(surf_selector *s);
surf_node *surf_selector_node(surf_selector *s);
void       surf_selector_set_color(surf_selector *s, surf_color c);
int32_t    surf_selector_index(const surf_selector *s);
int32_t    surf_selector_positions(const surf_selector *s);
void       surf_selector_set_index(surf_selector *s, int32_t idx);  /* no cb */
void       surf_selector_on_change(surf_selector *s, surf_index_cb cb,
                                   void *user);


#ifdef __cplusplus
}
#endif

#endif /* SURFER_H */
