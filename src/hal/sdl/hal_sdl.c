/* SDL2 backend: RGB565 software framebuffer + streaming texture. This file
 * is the only place outside build tools where per-pixel loops are allowed;
 * on device the same ops are PPA/2D-DMA jobs. */
/* posix_memalign, which strict C11 hides on glibc (macOS shows it anyway) */
#if !defined(_POSIX_C_SOURCE) && !defined(_WIN32)
#define _POSIX_C_SOURCE 200112L
#endif
#include <SDL.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef __APPLE__
#include <TargetConditionals.h>
#endif
#ifndef TARGET_OS_IPHONE
#define TARGET_OS_IPHONE 0
#endif
#if defined(__EMSCRIPTEN__) && !defined(SURF_HAL_SDL_NO_YIELD)
#include <emscripten.h>
EM_ASYNC_JS(void, surf_web_raf_yield, (void), {
    await new Promise((resolve) => requestAnimationFrame(resolve));
});
#endif

#include "hal_sdl.h"

/* content pixels per wheel notch: a flick should cover a list rather
 * than inch down it */
#define WHEEL_PX 48

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
    /* Real fingers, on a touchscreen behind SDL (a tablet, and — the
     * case this was added for — a phone browser). Slot index IS the
     * contact id the core sees, so it must be stable for the life of a
     * finger: SDL's own SDL_FingerID is an int64 that counts up for
     * ever, and surfer's per-contact table is five slots wide. */
    struct {
        SDL_FingerID id;
        int16_t      x, y;
        bool         used;
    } fing[SURF_MAX_CONTACTS];
    int16_t mouse_x, mouse_y;
    surf_rect     scrolled;     /* union of scroll_rect regions this frame */
    bool          has_scrolled;
    int           scale;        /* SURF_SCALE: integer nearest-neighbour zoom */
    int16_t       win_w, win_h; /* window size in POINTS, which is not the
                                 * framebuffer size under SURF_SCALE or
                                 * SURF_NATIVE — mouse events arrive here */
    int           out_w, out_h; /* renderer output (drawable) in real pixels */
    SDL_Rect      view;         /* where the fb lands inside the drawable */
    void (*on_interrupt)(void); /* Ctrl-C hook; NULL = ignore the key */
    bool          free_aspect;  /* SURF_FREE_ASPECT: let the window be any shape */
    uint64_t      snap_at;      /* when to put the window back on aspect; 0 = idle */
    int16_t       snap_w, snap_h;  /* the size BEFORE this drag started */
    int           kb_pt;        /* the platform screen keyboard's TOP edge in
                                 * window POINTS (iOS; 0 = no keyboard). The
                                 * top rather than the height, because iOS
                                 * 26's floating keyboard panel makes height
                                 * understate the footprint — the space
                                 * above the frame is the usable answer. The
                                 * OS shows and hides it without telling SDL
                                 * in any event, so the pump polls and the
                                 * view refits on a change */
    char          synth_ch;     /* iOS hardware keyboard: last character
                                 * synthesised from a key-down, so a text
                                 * event echoing it can be dropped */
    uint64_t      synth_at;
} S;

/* The window is resizable, so the framebuffer rarely covers the drawable
 * exactly. A window dragged to an exact multiple gets an exact
 * nearest-neighbour zoom; anywhere else gets the largest aspect-preserved
 * FIT, centred, with letterbox bars.
 *
 * Whole multiples ONLY is the tempting rule — every surfer pixel then
 * covers the same number of screen pixels, which is what this backend is
 * for — but it means a window dragged to 1.8x still draws at 1x with bars
 * round it, and nobody reads that as "not a whole multiple yet". They
 * read it as the view having collapsed. SURF_SCALE=N is there for a
 * guaranteed exact zoom. */
static void update_view(void)
{
    int ow = 0, oh = 0;
    SDL_GetRendererOutputSize(S.ren, &ow, &oh);
    if (ow <= 0 || oh <= 0)
        return;
    S.out_w = ow;
    S.out_h = oh;
    /* The platform screen keyboard (iOS) rises over the bottom of the
     * drawable and SDL neither resizes the window for it nor exposes
     * its height — SDL_tulip_kb_height_pt is our patch's export from
     * the uikit view controller, in window points. The view FITS INTO
     * WHAT THE KEYBOARD LEAVES: subtract it here and every rule below
     * (aspect fit, whole-multiple snap, centring) applies unchanged to
     * the space that can actually be seen. */
    int avail_h = oh;
    int top_px = 0;             /* the notch/island band, kept clear */
#if TARGET_OS_IPHONE
    {
        extern int SDL_tulip_kb_top_pt;
        extern int SDL_tulip_kb_height_pt;
        S.kb_pt = SDL_tulip_kb_top_pt;
        if (S.kb_pt > 0) {
            /* SCALE BY THE KEYBOARD FRAME'S OWN EXTENT, never by the
             * window size — SDL's window height disagrees with UIKit's
             * coordinate space in landscape (measured on an iPhone 17
             * Pro: SDL says 874x512 while the keyboard frame says the
             * screen is 402 tall, and 566+308 == 874 exactly in
             * portrait, so it is the LANDSCAPE window figure that is
             * wrong). Dividing by it shrank the machine by that ratio
             * and left a band of dead space above the keyboard.
             *
             * top + height is the screen's own height as UIKit measured
             * it in the same breath as the top, so the fraction is
             * self-consistent whatever SDL thinks. A keyboard that does
             * not reach the bottom (an iPad floating panel) makes that
             * sum short, which fits the view SMALLER — the safe way to
             * be wrong, and never off by a whole orientation. */
            int screen_pt = S.kb_pt + SDL_tulip_kb_height_pt;
            if (screen_pt > 0) {
                int top_px = (int)(((int64_t)S.kb_pt * oh) / screen_pt);
                if (top_px > 0 && top_px < avail_h)
                    avail_h = top_px;
            }
        }
        /* ...and the HOST's own chrome under that, if it has any.
         *
         * A FRACTION, not a height, and that is the whole point:
         * `surf_host_chrome_q16` is how much of the window the host
         * wants kept clear, Q16, measured by the host in its own
         * coordinate space. Passing points meant dividing by SDL's
         * window height — which on a real iPhone in landscape is 512
         * where UIKit says 402, so the bar came out 22% short of the
         * room it needed and sat ON the machine's task bar. The
         * keyboard fix above learned the same lesson; this one cannot
         * relearn it, because there is no denominator left to get
         * wrong. tulip2's iOS key bar (drivers/ios_bar.m) sets it. */
        if (surf_host_chrome_q16 > 0) {
            int bar_px = (int)(((int64_t)oh * surf_host_chrome_q16) >> 16);
            if (bar_px > 0 && bar_px < avail_h)
                avail_h -= bar_px;
        }
        /* ...and the same at the TOP, which is the notch or the island.
         * It cannot be bought with slack the way a caller might expect
         * — ask for a shorter framebuffer and the CENTRING pushes it
         * down — because there is no slack left when the screen
         * keyboard has taken the bottom of the window: the fit is
         * against a short band and lands hard against the top, under
         * the island. Reserved here, it is clear in both cases. */
        if (surf_host_chrome_top_q16 > 0) {
            int t = (int)(((int64_t)oh * surf_host_chrome_top_q16) >> 16);
            if (t > 0 && t < avail_h) {
                top_px = t;
                avail_h -= t;
            }
        }
    }
#endif
    oh = avail_h;
    /* Under SURF_NATIVE the framebuffer already fills the drawable at 1x,
     * so with a whole-multiple rule every resize short of an exact
     * DOUBLING left the view at 1x in the middle of a bigger window. And
     * because a host only presents when something is damaged, that shrink
     * showed up at the next keystroke rather than when the drag ended —
     * which reads as "it went small again when I typed". */
    int32_t q = (int32_t)(((int64_t)ow << 16) / S.w);
    int32_t qh = (int32_t)(((int64_t)oh << 16) / S.h);
    if (qh < q)
        q = qh;
    /* a drawable a hair off an exact zoom should BE that zoom, not
     * 1.99x with a seam every few hundred rows */
    if (q >= (1 << 16) && (q & 0xFFFF) < 1600)          /* within ~2.5% */
        q &= ~0xFFFF;
    S.view.w = (int)(((int64_t)S.w * q) >> 16);
    S.view.h = (int)(((int64_t)S.h * q) >> 16);
    if (S.view.w > ow) S.view.w = ow;
    if (S.view.h > oh) S.view.h = oh;
    S.view.x = (ow - S.view.w) / 2;
    S.view.y = top_px + (oh - S.view.h) / 2;

    if (getenv("SURF_VIEW_DEBUG"))
        fprintf(stderr, "VIEW drawable %dx%d fb %dx%d -> view %dx%d at %d,%d\n",
                ow, oh, S.w, S.h, S.view.w, S.view.h, S.view.x, S.view.y);
    int ww = 0, wh = 0;
    SDL_GetWindowSize(S.win, &ww, &wh);
    S.win_w = (int16_t)ww;
    S.win_h = (int16_t)wh;
}

/* SDL2 has no aspect-ratio constraint — SDL3 added one — so the window
 * can be dragged to any shape and the view just letterboxes inside it.
 * Dragging the bottom edge alone then grows a band of black rather than
 * the picture, which reads as the app being out of shape.
 *
 * So put the window back on the framebuffer's aspect after a resize,
 * keeping the axis the user actually dragged and deriving the other. It
 * happens on a short DELAY, once the drag has gone quiet: SDL's Cocoa
 * driver reports every intermediate size of a live drag, and resizing the
 * window from inside that stream fights the mouse and jitters. Waiting
 * for the stream to stop gives a single snap when you let go.
 *
 * SURF_FREE_ASPECT=1 turns it off for anyone who wants the bars. */
#define SNAP_QUIET_US 180000

static void snap_aspect(void)
{
    int ww = 0, wh = 0;
    SDL_GetWindowSize(S.win, &ww, &wh);
    if (ww <= 0 || wh <= 0)
        return;
    /* Against the size the drag STARTED from, not the current one: the
     * resize events have already been folded into win_w/win_h by
     * update_view, so comparing with those says "nothing moved" and the
     * snap undoes the drag instead of following it. */
    int dw = ww > S.snap_w ? ww - S.snap_w : S.snap_w - ww;
    int dh = wh > S.snap_h ? wh - S.snap_h : S.snap_h - wh;
    int want_w = ww, want_h = wh;
    if (dh > dw)                                  /* they dragged vertically */
        want_w = (int)(((int64_t)wh * S.w + S.h / 2) / S.h);
    else
        want_h = (int)(((int64_t)ww * S.h + S.w / 2) / S.w);
    if (want_w < 64) want_w = 64;
    if (want_h < 48) want_h = 48;
    if (want_w != ww || want_h != wh)
        SDL_SetWindowSize(S.win, want_w, want_h);
}

struct SDL_Window *surf_hal_sdl_window(void) { return S.win; }

void surf_hal_sdl_on_interrupt(void (*fn)(void))
{
    S.on_interrupt = fn;
}

static void push_key(uint8_t kind, bool shift, bool ctrl, const char *utf8)
{
    int next = (S.key_w + 1) % TOUCH_RING;
    if (next == S.key_r)
        return;
    surf_sdl_key *k = &S.keys[S.key_w];
    k->kind = kind;
    k->shift = shift;
    k->ctrl = ctrl;
    k->utf8[0] = 0;
    if (utf8) {
        strncpy(k->utf8, utf8, sizeof k->utf8 - 1);
        k->utf8[sizeof k->utf8 - 1] = 0;
    }
    S.key_w = next;
}

int surf_hal_sdl_keys_held(surf_sdl_key *out, int max)
{
    const Uint8 *st = SDL_GetKeyboardState(NULL);
    SDL_Keymod mod = SDL_GetModState();
    bool shift = (mod & KMOD_SHIFT) != 0;
    /* Reported here too, for the same reason shift is: the held set is a
     * SNAPSHOT of the keyboard, and one that answered "shift is down"
     * while staying silent about ctrl would be lying by omission. The
     * pad mapping ignores both. */
    bool ctrl = (mod & KMOD_CTRL) != 0;
    int n = 0;
    static const struct { int sc; uint8_t kind; } SPECIAL[] = {
        {SDL_SCANCODE_LEFT, SURF_KEY_LEFT},   {SDL_SCANCODE_RIGHT, SURF_KEY_RIGHT},
        {SDL_SCANCODE_UP, SURF_KEY_UP},       {SDL_SCANCODE_DOWN, SURF_KEY_DOWN},
        {SDL_SCANCODE_PAGEUP, SURF_KEY_PGUP}, {SDL_SCANCODE_PAGEDOWN, SURF_KEY_PGDN},
        {SDL_SCANCODE_HOME, SURF_KEY_HOME},   {SDL_SCANCODE_END, SURF_KEY_END},
        {SDL_SCANCODE_RETURN, SURF_KEY_ENTER},
        {SDL_SCANCODE_BACKSPACE, SURF_KEY_BACKSPACE},
        {SDL_SCANCODE_DELETE, SURF_KEY_DELETE},
    };
    for (size_t i = 0; i < sizeof SPECIAL / sizeof *SPECIAL && n < max; i++)
        if (st[SPECIAL[i].sc])
            out[n++] = (surf_sdl_key){.kind = SPECIAL[i].kind, .shift = shift, .ctrl = ctrl};
    if (st[SDL_SCANCODE_SPACE] && n < max)
        out[n++] = (surf_sdl_key){.kind = SURF_KEY_TEXT, .shift = shift, .ctrl = ctrl,
                                  .utf8 = {' '}};
    for (int i = 0; i < 26 && n < max; i++)
        if (st[SDL_SCANCODE_A + i])
            out[n++] = (surf_sdl_key){.kind = SURF_KEY_TEXT, .shift = shift, .ctrl = ctrl,
                                      .utf8 = {(char)((shift ? 'A' : 'a') + i)}};
    for (int i = 0; i < 10 && n < max; i++)
        if (st[SDL_SCANCODE_1 + i])
            out[n++] = (surf_sdl_key){.kind = SURF_KEY_TEXT, .shift = shift, .ctrl = ctrl,
                                      .utf8 = {"1234567890"[i]}};
    return n;
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
                          surf_rect vis, uint8_t rot, uint8_t mirror,
                          uint8_t opa)
{
    if (opa == 0)
        return;
    /* nearest-neighbor inverse mapping; rot = quarter turns CCW and
     * mirror flips the source before rotation, matching the P4 PPA's
     * SRM engine. dst_r is the post-rotation footprint; W0/H0 is the
     * footprint before rotation. */
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
            if (mirror & 1)
                ux = W0 - 1 - ux;
            if (mirror & 2)
                uy = H0 - 1 - uy;
            int32_t sx = sr.x + (int32_t)((int64_t)ux * sr.w / W0);
            int32_t sy = sr.y + (int32_t)((int64_t)uy * sr.h / H0);
            uint16_t *d = drow + vis.x + x;
            if (src->format == SURF_FMT_ARGB8888) {
                uint32_t p = *(const uint32_t *)((const uint8_t *)src->pixels +
                                                 sy * src->stride + sx * 4);
                *d = blend_px(*d, p, (p >> 24) * opa / 255);
            } else if (src->format == SURF_FMT_RGB565) {
                uint16_t p = *(const uint16_t *)((const uint8_t *)src->pixels +
                                                 sy * src->stride + sx * 2);
                if (opa == 255) {
                    *d = p;
                } else {  /* 565 carries no alpha; only opa applies */
                    uint32_t rgb = ((uint32_t)((p >> 8) & 0xf8) << 16) |
                                   ((uint32_t)((p >> 3) & 0xfc) << 8) |
                                   (uint32_t)((p << 3) & 0xf8);
                    *d = blend_px(*d, rgb, opa);
                }
            } else {  /* A8: alpha from image, color from tint */
                surf_color t = src->tint;
                uint32_t rgb = ((uint32_t)((t >> 8) & 0xf8) << 16) |
                               ((uint32_t)((t >> 3) & 0xfc) << 8) |
                               (uint32_t)((t << 3) & 0xf8);
                uint8_t a = *((const uint8_t *)src->pixels + sy * src->stride + sx);
                *d = blend_px(*d, rgb, (uint32_t)a * opa / 255);
            }
        }
    }
}

/* Repaint the window from the texture as it stands — for when the VIEW
 * moved and the picture did not. A host only presents on damage, so a
 * view that re-anchors (the screen keyboard going, a resize) with a
 * quiet framebuffer used to leave the picture drawn at the OLD place
 * while map_pt already answered for the new one: on an iPad the machine
 * sat at the top of the screen taking touches for the middle. The
 * "shrink showed up at the next keystroke" note in update_view was the
 * same gap on the desktop, priced smaller. */
static void view_present(void)
{
    if (!S.ren || !S.tex)
        return;
    SDL_RenderClear(S.ren);
    SDL_RenderCopy(S.ren, S.tex, NULL, &S.view);
    SDL_RenderPresent(S.ren);
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
    /* clear first: the letterbox bars are outside the view rect and
     * nothing else ever writes them */
    SDL_RenderClear(S.ren);
    SDL_RenderCopy(S.ren, S.tex, NULL, &S.view);
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

/* Every contact down right now: real fingers if there are any, and
 * otherwise the held mouse as contact 0. This is what `surfer.touches()`
 * reports, and the reason it is not just the event stream is that a
 * caller wants the CURRENT state — paint and voices' keyboard both ask
 * once a frame rather than tracking downs and ups themselves. */
static int h_touch_points(surf_touch_pt *out, int max)
{
    int n = 0;
    for (int i = 0; i < SURF_MAX_CONTACTS && n < max; i++)
        if (S.fing[i].used)
            out[n++] = (surf_touch_pt){S.fing[i].x, S.fing[i].y, (uint8_t)i};
    if (n == 0 && S.mouse_down && max > 0)
        out[n++] = (surf_touch_pt){S.mouse_x, S.mouse_y, 0};
    return n;
}

/* SDL's finger id is an int64 that counts up for ever; surfer's contact
 * table is five slots. `down` opens one, and anything else looks up a
 * finger already in flight. A DOWN for an id that is already here
 * reuses its slot rather than opening a second — the same rule the core
 * keeps, and for the same reason: a controller that misses an UP would
 * otherwise leak slots until nothing new can be pressed. */
static int finger_slot(SDL_FingerID id, bool down)
{
    for (int i = 0; i < SURF_MAX_CONTACTS; i++)
        if (S.fing[i].used && S.fing[i].id == id)
            return i;
    if (!down)
        return -1;
    for (int i = 0; i < SURF_MAX_CONTACTS; i++)
        if (!S.fing[i].used) {
            S.fing[i].used = true;
            S.fing[i].id = id;
            return i;
        }
    return -1;                  /* a sixth finger is dropped, not queued */
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

/* clock-based frame lock at 60/divisor fps (no real vsync headless;
 * the web build no-ops — requestAnimationFrame is the pacer there) */
static int64_t S_pace_next;

static void h_wait_frame(int divisor)
{
#ifndef SURF_HAL_SDL_NO_YIELD
    int64_t period = (int64_t)divisor * 16667;
    int64_t now = (int64_t)h_now_us();
    if (S_pace_next == 0 || now >= S_pace_next + period) {
        S_pace_next = now + period;   /* late or first: re-anchor */
        return;
    }
    while (now < S_pace_next) {
        int64_t left = S_pace_next - now;
        if (left > 2000)
            SDL_Delay((Uint32)((left - 1000) / 1000));
        now = (int64_t)h_now_us();
    }
    S_pace_next += period;
#else
    (void)divisor;
#endif
}

static float h_frame_hz(void)
{
    return 60.0f;   /* the clock pacer's base, not a real panel */
}

static void h_band_shift(surf_rect r, int16_t sx, int16_t sy)
{
    /* single software fb: an overlap-safe memmove per row; the present
     * scroll catch-up (has_scrolled) uploads the whole band after */
    if (sx == 0 && sy == 0)
        return;
    int16_t w = r.w - (int16_t)(sx < 0 ? -sx : sx);
    int16_t h = r.h - (int16_t)(sy < 0 ? -sy : sy);
    if (w <= 0 || h <= 0)
        return;
    int16_t src_x = (int16_t)(r.x + (sx < 0 ? -sx : 0));
    int16_t src_y = (int16_t)(r.y + (sy < 0 ? -sy : 0));
    int16_t dst_x = (int16_t)(r.x + (sx > 0 ? sx : 0));
    int16_t dst_y = (int16_t)(r.y + (sy > 0 ? sy : 0));
    if (dst_y <= src_y) {
        for (int y = 0; y < h; y++)
            memmove(S.fb + (dst_y + y) * S.w + dst_x,
                    S.fb + (src_y + y) * S.w + src_x, (size_t)w * 2);
    } else {
        for (int y = h - 1; y >= 0; y--)
            memmove(S.fb + (dst_y + y) * S.w + dst_x,
                    S.fb + (src_y + y) * S.w + src_x, (size_t)w * 2);
    }
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
}

static void *h_alloc_image(size_t bytes)
{
#ifdef _WIN32
    /* Windows has no posix_memalign; _aligned_malloc wants _aligned_free */
    return _aligned_malloc((bytes + 63) & ~(size_t)63, 64);
#else
    void *p = NULL;
    if (posix_memalign(&p, 64, (bytes + 63) & ~(size_t)63) != 0)
        return NULL;
    return p;
#endif
}

static void h_free_image(void *p)
{
#ifdef _WIN32
    _aligned_free(p);
#else
    free(p);
#endif
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
    .touch_points = h_touch_points,
    .alloc_image = h_alloc_image,
    .free_image = h_free_image,
    .fb_ptr = h_fb_ptr,
    .scroll_rect = h_scroll_rect,
    .band_shift = h_band_shift,
    .wait_frame = h_wait_frame,
    .frame_hz = h_frame_hz,
};

/* ---- host glue ---- */

/* x/y arrive in window points; scene space is the framebuffer. Going via
 * the drawable and the letterboxed view rather than dividing by
 * SURF_SCALE is what keeps a click landing on what it looks like it hit
 * after a resize, or under SURF_NATIVE where the window is SMALLER than
 * the framebuffer. */
/* Window points -> framebuffer pixels. Its own function because the
 * WHEEL needs it too and did not have it: that path took the pointer
 * straight from SDL_GetMouseState and hit-tested with it, so on any
 * display where the drawable is not the window 1:1 — every retina Mac —
 * it scrolled whatever was at roughly double the position of the thing
 * under the pointer. Invisible until something reported those
 * coordinates onward, which surf_input_wheel's queue now does. */
static void map_pt(int16_t *x, int16_t *y)
{
    if (S.win_w <= 0 || S.win_h <= 0 || S.view.w <= 0 || S.view.h <= 0)
        return;
    int px = (int)*x * S.out_w / S.win_w - S.view.x;
    int py = (int)*y * S.out_h / S.win_h - S.view.y;
    *x = (int16_t)(px * S.w / S.view.w);
    *y = (int16_t)(py * S.h / S.view.h);
}

static void push_touch(int16_t x, int16_t y, uint8_t phase, uint8_t id)
{
    int next = (S.ring_w + 1) % TOUCH_RING;
    if (next == S.ring_r)
        return;  /* full: drop; UP events still arrive next pump */
    map_pt(&x, &y);
    S.ring[S.ring_w] = (surf_touch){x, y, phase, id};  /* a mouse is contact 0 */
    S.ring_w = next;
}

#if TARGET_OS_IPHONE
/* A LATCHED CTRL FROM THE HOST'S KEY BAR, applied to one character.
 *
 * It has to mean exactly what ctrl+letter means on a desktop, or the
 * phone is a machine with different keys: every letter becomes its
 * control character (^S saves, ^Q quits, ^F finds) EXCEPT ^C, which is
 * the interrupt and never a keystroke — the same split the KEYDOWN
 * chord case makes above, and for the same reason: when a runaway loop
 * has the machine stuck, the key queue is not being read and the
 * interrupt hook is the only wire still working.
 *
 * Returns true if it consumed the character. The latch is one-shot
 * whatever happened: a modifier that survives its keystroke is one
 * nobody can turn off. */
static bool take_ctrl_latch(unsigned char c, void (*on_interrupt)(void))
{
    if (!surf_host_ctrl_latch)
        return false;
    surf_host_ctrl_latch = 0;
    if (c >= 'A' && c <= 'Z')
        c += 'a' - 'A';
    if (c < 'a' || c > 'z')
        return true;                 /* ctrl+punctuation: nothing to send */
    if (c == 'c') {
        if (on_interrupt)
            on_interrupt();
        return true;
    }
    char ctl[2] = {(char)(c - 'a' + 1), 0};
    push_key(SURF_KEY_TEXT, false, false, ctl);
    return true;
}
#endif

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

    /* SURF_SCALE=N asks for an N-times window, in points. Default 1: one
     * framebuffer pixel per point, which is the size this demo has always
     * been and the one to leave alone — SURF_NATIVE and SURF_SCALE are
     * both there for looking closer, not for relocating the baseline.
     * Nearest-neighbour is the whole point — the hint is set explicitly
     * rather than trusting SDL's default, because a smoothed upscale
     * invents edge pixels and makes every bake look antialiased. */
    S.free_aspect = getenv("SURF_FREE_ASPECT") != NULL;
    const char *se = getenv("SURF_SCALE");
    S.scale = se ? atoi(se) : 1;
    if (S.scale < 1) S.scale = 1;
    if (S.scale > 8) S.scale = 8;
    SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, "0");

    /* A zoom the desktop cannot hold is worse than no zoom: the window
     * runs off the screen and you lose the part you wanted to look at.
     * Clamp to the usable bounds and let update_view() pick the largest
     * whole multiple that fits — SURF_SCALE=2 on a display too small for
     * 2x then still gets you the biggest exact zoom there is room for,
     * rather than a 2048pt window on a 1710pt desktop. */
    int req_w = w * S.scale, req_h = h * S.scale;
    uint32_t flags = SDL_WINDOW_ALLOW_HIGHDPI;
#if TARGET_OS_IPHONE
    /* BORDERLESS ON iOS, AND THAT IS WHAT HIDES THE STATUS BAR. SDL's
     * view controller answers prefersStatusBarHidden from the window's
     * FULLSCREEN|BORDERLESS flags and nothing else — an Info.plist
     * UIStatusBarHidden does not reach it, because iOS 13+ resolves the
     * status bar through the view controller for a scene-based app. So
     * a plain window kept the clock and the battery drawn over the
     * machine's top rows: harmless where the view is letterboxed, and
     * on an iPad with the keyboard up (where the view fills to the top)
     * it sat over the world app's tab strip. A phone or tablet app IS
     * the whole screen; there is no border for it to have. */
    flags |= SDL_WINDOW_BORDERLESS;
    /* ...AND THE EDGE GESTURES STAY THE SYSTEM'S. SDL reads the same
     * two flags for preferredScreenEdgesDeferringSystemGestures and
     * gives a borderless window UIRectEdgeAll by default, so asking for
     * a hidden status bar silently opted the machine into DEFERRING
     * every edge swipe: the home-indicator swipe up needed one gesture
     * to arm it and another to act, reported as "I have to swipe a few
     * times to switch apps". A machine somebody keeps a REPL in is not
     * a game that must not be swiped out of by accident — the system's
     * gestures are how they leave, and they should work first time.
     * "0" means show the indicator and defer nothing (only "2" defers;
     * see SDL_uikitviewcontroller's rule). */
    SDL_SetHint(SDL_HINT_IOS_HIDE_HOME_INDICATOR, "0");
#endif
#ifndef __EMSCRIPTEN__
    flags |= SDL_WINDOW_RESIZABLE;
    SDL_Rect usable;
    if (SDL_GetDisplayUsableBounds(0, &usable) == 0 &&
        usable.w > 0 && usable.h > 0) {
        if (req_w > usable.w) req_w = usable.w;
        if (req_h > usable.h) req_h = usable.h;
    }
#endif
    S.win = SDL_CreateWindow(title, SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
                             req_w, req_h, flags);
    if (!S.win)
        goto fail;
    S.win_w = (int16_t)req_w;
    S.win_h = (int16_t)req_h;
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

    /* SURF_NATIVE=1: one framebuffer pixel per PHYSICAL display pixel.
     *
     * A surfer pixel drawn one-per-point lands at 110-140dpi depending on
     * the display-scaling setting — a fair copy of a pre-retina desktop,
     * but always COARSER than the P4's 7" 1024x600 panel (169dpi), often
     * by 1.5x. Every jaggy and AA fringe on screen is therefore bigger
     * than anything the device will ever show, which is most of why
     * bitmap faces look worse here than on hardware. Shrinking the window
     * by the drawable ratio puts one surfer pixel on one ~220dpi pixel:
     * denser than the panel rather than coarser, so it errs the other
     * way. No resampling either way — the texture stays 1:1 with real
     * pixels. The truth is between the two and neither is reachable.
     *
     * This is an absolute density, not a multiplier, so it OVERRIDES
     * SURF_SCALE rather than composing with it — the two are competing
     * answers to "how big", and this one is the answer in device pixels. */
    if (getenv("SURF_NATIVE")) {
        int dw = 0, dh = 0, ww = 0, wh = 0;
        SDL_GetRendererOutputSize(S.ren, &dw, &dh);
        SDL_GetWindowSize(S.win, &ww, &wh);
        if (ww > 0 && wh > 0 && dw > ww && dh > wh) {
            S.win_w = (int16_t)(w * ww / dw);
            S.win_h = (int16_t)(h * wh / dh);
            SDL_SetWindowSize(S.win, S.win_w, S.win_h);
            fprintf(stderr, "surfer: SURF_NATIVE: %dx%d fb in a %dx%d pt "
                            "window (display is %dx backing)\n",
                    w, h, S.win_w, S.win_h, dw / ww);
        } else {
            fprintf(stderr, "surfer: SURF_NATIVE: display is already 1x, "
                            "nothing to shrink\n");
        }
    }

    S.tex = SDL_CreateTexture(S.ren, SDL_PIXELFORMAT_RGB565,
                              SDL_TEXTUREACCESS_STREAMING, w, h);
    S.fb = h_alloc_image((size_t)w * h * 2);
    if (!S.tex || !S.fb)
        goto fail;
    memset(S.fb, 0, (size_t)w * h * 2);
    SDL_SetRenderDrawColor(S.ren, 0, 0, 0, 255);   /* the letterbox bars */
    update_view();
    SDL_StartTextInput();
    return &hal_sdl;
fail:
    surf_hal_sdl_quit();
    return NULL;
}

/* The platform screen keyboard, for hosts whose glass is the keyboard
 * (iOS; a desktop answers -1 and a caller draws no toggle). op -1 asks,
 * 1 summons, 0 dismisses; the answer is always what is actually shown.
 * Summoning is Stop THEN Start deliberately: the user can dismiss the
 * keyboard from its own key without SDL noticing, after which text
 * input is still nominally active and a plain Start is a no-op — the
 * bounce through Stop is what makes the keyboard come back. */
int surf_screen_keyboard(int op)
{
    if (!S.win || !SDL_HasScreenKeyboardSupport())
        return -1;
    if (op == 1) {
        SDL_StopTextInput();
        SDL_StartTextInput();
    } else if (op == 0) {
        SDL_StopTextInput();
    }
    /* the view refit rides the height poll in the pump, next frame */
    return SDL_IsScreenKeyboardShown(S.win) ? 1 : 0;
}

bool surf_hal_sdl_resize(int16_t w, int16_t h)
{
    if (!S.win || !S.ren || w <= 0 || h <= 0)
        return false;
    if (w == S.w && h == S.h)
        return true;
    /* BUILD BOTH BEFORE FREEING EITHER, so a failure leaves the caller
     * with the display it already had rather than none. */
    SDL_Texture *tex = SDL_CreateTexture(S.ren, SDL_PIXELFORMAT_RGB565,
                                         SDL_TEXTUREACCESS_STREAMING, w, h);
    void *fb = h_alloc_image((size_t)w * h * 2);
    if (!tex || !fb) {
        if (tex) SDL_DestroyTexture(tex);
        if (fb) h_free_image(fb);
        return false;
    }
    SDL_DestroyTexture(S.tex);
    h_free_image(S.fb);
    S.tex = tex;
    S.fb = fb;
    S.w = w;
    S.h = h;
    memset(S.fb, 0, (size_t)w * h * 2);
    /* The WINDOW is told too, and on iOS that is the load-bearing half
     * rather than cosmetic: UIKit_GetSupportedOrientations derives the
     * orientations the app is allowed from the window's own aspect when
     * the window is not resizable (SDL_uikitwindow.c), so a machine that
     * has just become landscape stays locked to portrait until this
     * lands. A fullscreen window ignores the size itself and keeps the
     * screen's; what changes is what SDL believes it asked for. */
    SDL_SetWindowSize(S.win, w, h);
    update_view();
    return true;
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
    if (S.view.w < S.w || S.view.h < S.h)
        return false;   /* zoomed below 1:1; a readback would lose rows */
    uint16_t *px = malloc((size_t)ow * oh * 2);
    if (!px)
        return false;
    SDL_RenderClear(S.ren);
    SDL_RenderCopy(S.ren, S.tex, NULL, &S.view);
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
            /* nearest sample out of the letterboxed view, which is where
             * the hidpi and zoom scaling both ended up */
            uint16_t p = px[(int64_t)(S.view.y + y * S.view.h / S.h) * ow +
                            S.view.x + x * S.view.w / S.w];
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
    /* The drag has gone quiet: put the window back on aspect. Not while a
     * mouse button is still down, though — pausing mid-drag would other-
     * wise yank the window out from under the pointer. */
    if (S.snap_at && h_now_us() >= S.snap_at) {
        if (SDL_GetGlobalMouseState(NULL, NULL) &
            (SDL_BUTTON_LMASK | SDL_BUTTON_RMASK | SDL_BUTTON_MMASK)) {
            S.snap_at = h_now_us() + SNAP_QUIET_US;
        } else {
            S.snap_at = 0;
            snap_aspect();
        }
    }
    /* The screen keyboard's comings and goings generate no SDL event —
     * the user can dismiss it from its own key — so its height is
     * polled here and a change refits the view into the space left.
     * One integer compare per pump. */
#if TARGET_OS_IPHONE
    {
        extern int SDL_tulip_kb_top_pt;
        extern int SDL_tulip_kb_height_pt;
        /* The HOST's chrome moves without a keyboard event of its own —
         * it is created after the window and re-measures on every
         * rotation — so its fraction is watched here too. Without this
         * the view kept whatever the bar reported FIRST (a portrait
         * fraction, on a landscape launch) and the strip sat over the
         * machine's task bar until something else happened to refit. */
        static int last_chrome = -1, last_chrome_top = -1;
        if (S.win && (surf_host_chrome_q16 != last_chrome ||
                      surf_host_chrome_top_q16 != last_chrome_top)) {
            last_chrome = surf_host_chrome_q16;
            last_chrome_top = surf_host_chrome_top_q16;
            update_view();
            view_present();
        }
        if (S.win && SDL_tulip_kb_top_pt != S.kb_pt) {
            if (getenv("SURF_VIEW_DEBUG"))
                fprintf(stderr, "surfer: kb top=%d h=%d win=%dx%d\n",
                        SDL_tulip_kb_top_pt, SDL_tulip_kb_height_pt,
                        S.win_w, S.win_h);
            update_view();          /* reads and records the new top */
            view_present();
        }
    }
#endif
    SDL_Event e;
    while (SDL_PollEvent(&e)) {
        switch (e.type) {
        case SDL_QUIT:
            return false;
        case SDL_WINDOWEVENT:
            /* SIZE_CHANGED covers both a drag and the hidpi backing
             * changing under us (dragging to a 1x monitor), which is the
             * case a plain RESIZED would miss. */
            if (e.window.event == SDL_WINDOWEVENT_SIZE_CHANGED) {
                if (!S.snap_at) {           /* first event of this drag */
                    S.snap_w = S.win_w;
                    S.snap_h = S.win_h;
                }
                update_view();
                view_present();     /* the picture must move WITH the
                                     * view, not at the next keystroke */
                if (!S.free_aspect)
                    S.snap_at = h_now_us() + SNAP_QUIET_US;
            }
            break;
        case SDL_KEYDOWN: {
            bool shift = (e.key.keysym.mod & KMOD_SHIFT) != 0;
            /* Carried on the named keys below, NOT on the ctrl+letter
             * chords, which already say it in their control character.
             * See surf_sdl_key in hal_sdl.h. */
            bool ctrl = (e.key.keysym.mod & KMOD_CTRL) != 0;
            /* ctrl/alt+Tab is its own key, not a Tab. Tab is not a letter,
             * so it does not fall into the ctrl+letter rule below, and
             * without this it arrives as an ordinary 0x09 — a host that
             * wants "next tab"/"next window" cannot tell the chord from
             * an indent. There is no standard control character for it,
             * so it gets 0x1e (RS), which nothing else emits.
             *
             * BOTH modifiers, because neither is portable alone: on Linux
             * and Windows alt+Tab belongs to the window manager and never
             * reaches us, while on macOS the system switcher is cmd+Tab
             * and alt+Tab does. ctrl+Tab is free everywhere and is the
             * one to document; alt+Tab is accepted where it survives. */
            if ((e.key.keysym.mod & (KMOD_CTRL | KMOD_ALT)) &&
                e.key.keysym.sym == SDLK_TAB) {
                push_key(SURF_KEY_TEXT, false, false, "\x1e");
                break;
            }
            if (e.key.keysym.mod & KMOD_CTRL) {
                bool chord = true;
                switch (e.key.keysym.sym) {
                /* Ctrl-C is the universal interrupt, not a keystroke: it
                 * has to reach the host even when the host is inside a
                 * loop that never reads the key queue — that IS the case
                 * it exists for. Swallowed either way, so no app ever
                 * sees a ^C. */
                case SDLK_c:
                    if (S.on_interrupt)
                        S.on_interrupt();
                    break;
                /* readline's line editing, which every terminal has and
                 * laptops without Home/End keys depend on. Delivered AS
                 * Home/End so nothing downstream needs chord handling:
                 * the REPL, textinput and any consumer already handle
                 * those. */
                case SDLK_a: push_key(SURF_KEY_HOME, shift, false, NULL); break;
                case SDLK_e: push_key(SURF_KEY_END, shift, false, NULL); break;
                default:
                    /* Every other ctrl+letter becomes its control
                     * character, which is what a terminal puts on the
                     * wire: ^S is 0x13, ^Q 0x11. Consumers that want
                     * terminal semantics (the editor's commands, an ssh
                     * session) get them for free; consumers that don't
                     * were dropping these keystrokes entirely before. */
                    if (e.key.keysym.sym >= SDLK_a && e.key.keysym.sym <= SDLK_z) {
                        char ctl[2] = {(char)(e.key.keysym.sym - SDLK_a + 1), 0};
                        push_key(SURF_KEY_TEXT, false, false, ctl);
                    } else {
                        chord = false;   /* ctrl+arrow still arrows */
                    }
                    break;
                }
                if (chord)
                    break;
            }
            /* BY SCANCODE, NOT KEYCODE, and that is a browser bug fix
             * rather than a style choice.
             *
             * A scancode is the physical key; a keycode is what that key
             * MEANS under the current layout, which SDL derives. On the
             * desktop both are populated and either works. Under
             * emscripten the derivation is not reliable for keys with no
             * character — so `keysym.sym` came back as something other
             * than SDLK_LEFT and every arrow was dropped.
             *
             * The symptom was beautifully specific and is what found it:
             * arrows moved a GAMEPAD in padtest but not the caret in the
             * editor or the REPL. Those are two different paths —
             * surf_hal_sdl_keys_held() feeds the pad and has always read
             * SDL_GetKeyboardState() by SCANCODE, while this queue fed
             * everything else by keycode. The two disagreeing is the
             * whole of the bug, so now they agree by construction.
             *
             * There is no layout ambiguity to lose: an arrow, Home, End,
             * Enter and Backspace are the same physical key everywhere.
             * ctrl+letter above still uses the keycode, which is right —
             * WHICH letter is exactly the layout-dependent part. */
            switch (e.key.keysym.scancode) {
            /* NOT `return false`. That closed the window from inside
             * the pump, which is a fine way for a C demo to exit and a
             * terrible one for a host: on tulip2 it took down the REPL,
             * every running app and anything unsaved. Esc is a key; what
             * it MEANS is the host's business. The demos close on the
             * window button and on ctrl+C as they always could. */
            case SDL_SCANCODE_ESCAPE:    push_key(SURF_KEY_ESC, shift, ctrl, NULL); break;
            case SDL_SCANCODE_LEFT:      push_key(SURF_KEY_LEFT, shift, ctrl, NULL); break;
            case SDL_SCANCODE_RIGHT:     push_key(SURF_KEY_RIGHT, shift, ctrl, NULL); break;
            case SDL_SCANCODE_UP:        push_key(SURF_KEY_UP, shift, ctrl, NULL); break;
            case SDL_SCANCODE_DOWN:      push_key(SURF_KEY_DOWN, shift, ctrl, NULL); break;
            case SDL_SCANCODE_PAGEUP:    push_key(SURF_KEY_PGUP, shift, ctrl, NULL); break;
            case SDL_SCANCODE_PAGEDOWN:  push_key(SURF_KEY_PGDN, shift, ctrl, NULL); break;
            case SDL_SCANCODE_HOME:      push_key(SURF_KEY_HOME, shift, ctrl, NULL); break;
            case SDL_SCANCODE_END:       push_key(SURF_KEY_END, shift, ctrl, NULL); break;
            case SDL_SCANCODE_BACKSPACE: push_key(SURF_KEY_BACKSPACE, shift, ctrl, NULL); break;
            case SDL_SCANCODE_DELETE:    push_key(SURF_KEY_DELETE, shift, ctrl, NULL); break;
            case SDL_SCANCODE_RETURN:
            case SDL_SCANCODE_KP_ENTER:  push_key(SURF_KEY_ENTER, shift, ctrl, NULL); break;
            /* Tab as text, the way a terminal delivers it. SDL_TEXTINPUT
             * does emit 0x09 on some platforms and not others, and the
             * control-character filter drops it either way — so it has to
             * come from here to arrive at all. */
            case SDL_SCANCODE_TAB: push_key(SURF_KEY_TEXT, shift, ctrl, "\t"); break;
            default:
#if TARGET_OS_IPHONE
                /* A HARDWARE keyboard on iOS (a keyboard case) delivers
                 * key-downs through GCKeyboard and its characters reach
                 * SDL's hidden text field not at all — so on the iPad
                 * only the named keys above did anything and every
                 * letter fell on the floor. Synthesise the text from the
                 * key-down the way the P4's usb_input.c does from HID:
                 * keysym.sym is the unshifted ASCII under the current
                 * layout, and the US shift table covers the rest. If
                 * some path DOES also deliver the SDL_TEXTINPUT, the
                 * dedupe in that case below drops the echo. */
                if (!(e.key.keysym.mod & (KMOD_CTRL | KMOD_GUI | KMOD_ALT))
                        && e.key.keysym.sym >= 0x20
                        && e.key.keysym.sym < 0x7f) {
                    char ch = (char)e.key.keysym.sym;
                    /* the host bar's latched Ctrl applies to a HARDWARE
                     * key too — a keyboard case has ctrl of its own, but
                     * the bar is on screen either way and a button that
                     * works only sometimes is worse than no button */
                    if (surf_host_ctrl_latch) {
                        S.synth_ch = ch;         /* swallow the text echo */
                        S.synth_at = h_now_us();
                        take_ctrl_latch((unsigned char)ch, S.on_interrupt);
                        break;
                    }
                    if (shift) {
                        if (ch >= 'a' && ch <= 'z') {
                            ch -= 32;
                        } else {
                            static const char *from = "1234567890-=[]\\;',./`";
                            static const char *to = "!@#$%^&*()_+{}|:\"<>?~";
                            const char *p = strchr(from, ch);
                            if (p)
                                ch = to[p - from];
                        }
                    }
                    S.synth_ch = ch;
                    S.synth_at = h_now_us();
                    char txt[2] = {ch, 0};
                    push_key(SURF_KEY_TEXT, shift, false, txt);
                }
#endif
                break;           /* -Wswitch: SDL_Scancode is an enum */
            }
            break;
        }
        case SDL_TEXTINPUT:
#if TARGET_OS_IPHONE
            /* the synthesised character's own echo, if a text event does
             * arrive for a hardware key after all — one character, equal,
             * and hot on the key-down's heels */
            if (S.synth_ch && e.text.text[0] == S.synth_ch &&
                    e.text.text[1] == 0 &&
                    h_now_us() - S.synth_at < 250000) {
                S.synth_ch = 0;
                break;
            }
#endif
#if TARGET_OS_IPHONE
            /* A LATCHED CTRL FROM THE HOST'S KEY BAR turns the next
             * letter into its control character — the same thing the
             * desktop's ctrl+letter case does, for a keyboard that HAS
             * no ctrl key. Without it ^C, ^S and ^Q are unreachable on
             * a phone, which is most of an editor's command set. The
             * latch is one-shot: consumed here, whatever the key was,
             * because a modifier that survives its keystroke is one
             * nobody can turn off. */
            if (take_ctrl_latch((unsigned char)e.text.text[0], S.on_interrupt))
                break;
#endif
            /* Drop control characters: some platforms deliver a text
             * event alongside a Ctrl-chord, and a raw ^C landing in an
             * edited line is a stray glyph nobody typed. UTF-8 lead
             * bytes are all >= 0xc2, so this only ever cuts ASCII. */
            if ((unsigned char)e.text.text[0] >= 0x20)
                push_key(SURF_KEY_TEXT, false, false, e.text.text);
            break;
        /* A REAL TOUCHSCREEN, which on this backend means a phone or a
         * tablet browser: emscripten's SDL turns page touches into
         * SDL_FINGER* and synthesises a mouse from the PRIMARY one only,
         * so before this a second finger simply did not exist. The core
         * has been per-contact for a while (surf_g.contacts, five slots,
         * a widget follows one finger) — the hal was the half that never
         * fed it, so three fingers on three faders worked on the panel
         * and not in a tab.
         *
         * DIRECT devices only. A mac trackpad is an SDL touch device too
         * (INDIRECT_ABSOLUTE), and resting a palm on it would otherwise
         * inject contacts into whatever is on screen — a laptop's
         * trackpad is a mouse here and a wheel, nothing else.
         *
         * Coordinates arrive NORMALISED to the window; push_touch wants
         * window points, and does the letterbox mapping from there. */
        case SDL_FINGERDOWN:
        case SDL_FINGERMOTION:
        case SDL_FINGERUP: {
            if (SDL_GetTouchDeviceType(e.tfinger.touchId) !=
                SDL_TOUCH_DEVICE_DIRECT)
                break;
            bool down = (e.type == SDL_FINGERDOWN);
            int slot = finger_slot(e.tfinger.fingerId, down);
            if (slot < 0)
                break;
            int16_t x = (int16_t)(e.tfinger.x * (float)S.win_w);
            int16_t y = (int16_t)(e.tfinger.y * (float)S.win_h);
            /* The contact table hands these straight to touches(), so
             * they must be CANVAS coordinates like everything else —
             * push_touch maps its own copy for the dispatch ring, and
             * storing the unmapped window points here was invisible on
             * every target whose window IS the canvas (desktop, web)
             * and wrong by the whole letterbox on the first one whose
             * window is the screen (iOS): the virtual pad read fingers
             * hundreds of pixels from where they were. */
            int16_t cx = x, cy = y;
            map_pt(&cx, &cy);
            S.fing[slot].x = cx;
            S.fing[slot].y = cy;
            push_touch(x, y, down ? SURF_TOUCH_DOWN
                                  : (e.type == SDL_FINGERUP ? SURF_TOUCH_UP
                                                            : SURF_TOUCH_MOVE),
                       (uint8_t)slot);
            if (e.type == SDL_FINGERUP)
                S.fing[slot].used = false;
            break;
        }
        case SDL_MOUSEBUTTONDOWN:
            /* SDL sends a synthetic mouse for the primary finger as
             * well. Taking both would make one finger two contacts —
             * and the second would never lift cleanly. */
            if (e.button.which == SDL_TOUCH_MOUSEID)
                break;
            if (e.button.button == SDL_BUTTON_LEFT) {
                S.mouse_down = true;
                /* S.mouse_x feeds touches() and so holds CANVAS
                 * coordinates — the finger table's rule; push_touch maps
                 * its own copy. Storing window points here was invisible
                 * in a 1:1 window and wrong under SURF_SCALE and on any
                 * window that letterboxes (iOS). */
                int16_t mx = (int16_t)e.button.x, my = (int16_t)e.button.y;
                push_touch(mx, my, SURF_TOUCH_DOWN, 0);
                map_pt(&mx, &my);
                S.mouse_x = mx;
                S.mouse_y = my;
            }
            break;
        case SDL_MOUSEMOTION:
            if (e.motion.which == SDL_TOUCH_MOUSEID)
                break;
            if (S.mouse_down) {
                int16_t mx = (int16_t)e.motion.x, my = (int16_t)e.motion.y;
                push_touch(mx, my, SURF_TOUCH_MOVE, 0);
                map_pt(&mx, &my);
                S.mouse_x = mx;
                S.mouse_y = my;
            }
            break;
        case SDL_MOUSEWHEEL: {
            /* A trackpad's two fingers, or a wheel. SDL reports lines,
             * not pixels, on this path — WHEEL_PX is what one notch
             * moves, chosen so a flick covers a list rather than
             * inching. Direction: content follows the fingers, which is
             * what every list on this machine does under a drag. */
            int mx = 0, my = 0;
            SDL_GetMouseState(&mx, &my);
            int16_t wx = (int16_t)mx, wy = (int16_t)my;
            map_pt(&wx, &wy);   /* the same mapping every click gets */
            surf_input_wheel(wx, wy, (int16_t)(-e.wheel.x * WHEEL_PX),
                             (int16_t)(-e.wheel.y * WHEEL_PX));
            break;
        }
        case SDL_MOUSEBUTTONUP:
            if (e.button.which == SDL_TOUCH_MOUSEID)
                break;
            if (e.button.button == SDL_BUTTON_LEFT) {
                S.mouse_down = false;
                push_touch((int16_t)e.button.x, (int16_t)e.button.y, SURF_TOUCH_UP, 0);
            }
            break;
        }
    }
    return true;
}
