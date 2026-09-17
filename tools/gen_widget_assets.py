#!/usr/bin/env python3
"""Build-time placeholder widget art for M1.

Emits a C header on stdout: a 64-frame knob filmstrip (indicator sweeping
-135°..+135°, matching the knob widget's angular mapping), a 9-patch
slider track, and a slider cap. All straight-alpha ARGB8888. Real themes
come from Blender renders via surfpack later (DESIGN.md §2.4).
"""
import math
import sys

KNOB_FRAMES = 64
KNOB = 64
KNOB_SM = 40  # small knob for dense panels (drum machines etc.)
TRACK = 36
TRACK_INSET = 12
TRACKFULL_W, TRACKFULL_H = 48, 330  # baked at the mixer's exact size: 1 blit
# A fader cap is TALLER than it is wide — a block you pinch, not a tab.
# 30 x 56 is 4.5 x 8.4 mm on the P4's 169 dpi panel, which clears every
# fingertip guideline there is; the old 40 x 20 was 3.0 mm tall.
CAP_W, CAP_H = 30, 56

# The COMPACT slider, for a panel with no room for a mixer fader: a thin
# bar with a small rounded-rectangle handle riding ACROSS it, wider than
# the bar so the overhang is what you read the value off.
#
# 24 x 14 is 3.6 x 2.1 mm on the P4's 169 dpi panel, under every
# fingertip guideline there is — which is the trade a dense panel makes,
# and the reason the full-size cap above is still the default. It is a
# smaller trade than it looks: the whole widget box is the grab area
# (slider.c clips its group), the cap centres on the finger, and a tap
# anywhere on the track jumps to it — so what a finger has to hit is the
# slider's declared WIDTH, never the handle.
SLIM_TRACK_W, SLIM_TRACK_H = 8, 14
SLIM_TRACK_INSET = 5
SLIM_CAP_W, SLIM_CAP_H = 24, 14


def clamp(v, lo=0, hi=255):
    return max(lo, min(hi, int(v)))


def ink(col, a):
    """An ARGB colour + coverage as ONE A8 byte.

    A8 art is a silhouette whose alpha the widget's `tint` colours, so
    everything a colour image says with LIGHTNESS this has to say with
    OPACITY: a bright pointer becomes opaque, a dark rim becomes
    see-through. That is the trade the format makes and the whole reason
    one asset can be any colour at no cost — on the P4 the tint is a
    palette register the PPA applies during the blend it was doing
    anyway (see led.c).

    It also means this art is drawn for a DARK panel: where the colour
    version put a dark edge, this puts background. Rec.601 luma, because
    it is what the eye reads as brightness."""
    lum = (col[0] * 77 + col[1] * 150 + col[2] * 29) >> 8
    return clamp(a * lum)


def mix(a, b, t):
    return tuple(clamp(x + (y - x) * t) for x, y in zip(a, b))


def seg_dist(px, py, ax, ay, bx, by):
    vx, vy = bx - ax, by - ay
    t = max(0.0, min(1.0, ((px - ax) * vx + (py - ay) * vy) / (vx * vx + vy * vy)))
    return math.hypot(px - (ax + vx * t), py - (ay + vy * t))


def knob_strip(size=KNOB):
    c = size / 2.0
    body_r = c - 5.0        # scale the 64px proportions down
    edge_r = c - 0.5
    ptr_w = max(1.8, size / 24.0)
    out = []
    frames = []
    for f in range(KNOB_FRAMES):
        ang = math.radians(-135.0 + 270.0 * f / (KNOB_FRAMES - 1))
        dx, dy = math.sin(ang), -math.cos(ang)
        frames.append((c + dx * size * 0.125, c + dy * size * 0.125,
                       c + dx * size * 0.345, c + dy * size * 0.345))
    for y in range(size):
        for f in range(KNOB_FRAMES):
            ax, ay, bx, by = frames[f]
            for x in range(size):
                px, py = x + 0.5, y + 0.5
                r = math.hypot(px - c, py - c)
                a = max(0.0, min(1.0, (edge_r - r) * 2.0))
                if a == 0.0:
                    out.append((f, x, y, 0))
                    continue
                if r > body_r:
                    col = (58, 62, 74)
                else:
                    sh = 1.0 - 0.45 * (r / body_r) ** 2
                    hr = math.hypot((px - c) / body_r + 0.35, (py - c) / body_r + 0.35)
                    spec = max(0.0, 1.0 - hr * 1.8) ** 3 * 140.0
                    col = tuple(clamp(v * sh + spec) for v in (126, 130, 142))
                d = seg_dist(px, py, ax, ay, bx, by)
                col = mix(col, (240, 242, 248),
                          max(0.0, min(1.0, ptr_w + 0.5 - d)))
                out.append((f, x, y, ink(col, a)))
    strip_w = size * KNOB_FRAMES
    px = [0] * (strip_w * size)
    for f, x, y, word in out:
        px[y * strip_w + f * size + x] = word
    return px


def button_patch(pressed):
    """9-patch for the button widget: raised normal, sunken pressed."""
    size = 18
    out = []
    for y in range(size):
        for x in range(size):
            px, py = x + 0.5, y + 0.5
            d, a = rounded_alpha(px, py, size, size, 5)
            if a == 0.0:
                out.append(0)
                continue
            if pressed:
                col = (40, 44, 54)
                if d > -1.6:
                    col = (70, 76, 90)
            else:
                col = (68, 74, 88)
                if py < 5:
                    col = mix(col, (96, 103, 120), (5 - py) / 5.0)
                if d > -1.6:
                    col = (104, 112, 130)
            out.append((clamp(a * 255) << 24) | (col[0] << 16) | (col[1] << 8) | col[2])
    return out


# The TAB. Rounded at the TOP, dead flat at the bottom, because that is
# the whole grammar of a tab: it is a card whose bottom edge is the page
# it belongs to, so the join has to be seamless and the free corners have
# to be the ones sticking up. A button's 9-patch is rounded all round and
# reads as a button sitting near a panel, which is exactly the note this
# came back with.
#
# A8, so the colour is the CALLER'S: the selected tab is drawn in the
# page's own background and disappears into it, and the others in
# something darker. One asset, two tints, and a caller can theme both
# without a new bake.
TAB_W, TAB_H = 24, 24
TAB_R = 8                    # top-corner radius
TAB_INSET_TOP = TAB_R + 2    # the 9-patch must not stretch the curve
TAB_INSET_BOT = 2


def tab_patch():
    out = []
    for y in range(TAB_H):
        for x in range(TAB_W):
            px, py = x + 0.5, y + 0.5
            # ONE distance field: how far outside a box whose top corners
            # are round. qx is the overshoot past the inner x-range, qy
            # the overshoot ABOVE the inner top edge only — never below,
            # so the foot is square and bleeds into the page.
            qx = max(TAB_R - px, px - (TAB_W - TAB_R), 0.0)
            qy = max(TAB_R - py, 0.0)
            d = math.hypot(qx, qy) - TAB_R
            out.append(clamp(max(0.0, min(1.0, 0.5 - d)) * 255))
    return out


def rounded_alpha(px, py, w, h, radius):
    qx = abs(px - w / 2.0) - (w / 2.0 - radius)
    qy = abs(py - h / 2.0) - (h / 2.0 - radius)
    d = math.hypot(max(qx, 0.0), max(qy, 0.0)) + min(max(qx, qy), 0.0) - radius
    return d, max(0.0, min(1.0, 0.5 - d))


def track(w, h):
    out = []
    for y in range(h):
        for x in range(w):
            px, py = x + 0.5, y + 0.5
            d, a = rounded_alpha(px, py, w, h, 8)
            if a == 0.0:
                out.append(0)
                continue
            col = (54, 58, 68)
            if d > -1.8:
                col = (84, 90, 104)
            groove = max(0.0, min(1.0, 5.5 - abs(px - w / 2.0)))
            col = mix(col, (32, 35, 42), groove)
            out.append((clamp(a * 255) << 24) | (col[0] << 16) | (col[1] << 8) | col[2])
    return out


# Grooves cut across the cap, as fractions of its height: (centre,
# half-height). Three above the index line and three below, the way a
# mixer fader's mouldings run.
CAP_GROOVES = ((0.135, 0.045), (0.265, 0.045), (0.395, 0.045),
               (0.605, 0.045), (0.735, 0.045), (0.865, 0.045))
CAP_INDEX = (0.500, 0.055)      # the line you read the value off


def cap():
    """The fader cap: a tall rounded block with horizontal grooves cut
    across it and a bright index line at the middle.

    A8 like the knob, so a slider's handle takes a colour for free (see
    ink()). Which way round the tones go matters and got flipped once:
    the BODY is the ink — near-opaque, so the cap reads as a solid
    coloured block — and the grooves are where the alpha drops away and
    the dark panel shows through, which is exactly what a groove looks
    like. Making the ridges the ink instead gave a ghost of a cap with a
    few bright stripes floating in it."""
    out = []
    for y in range(CAP_H):
        for x in range(CAP_W):
            px, py = x + 0.5, y + 0.5
            d, a = rounded_alpha(px, py, CAP_W, CAP_H, 5)
            if a == 0.0:
                out.append(0)
                continue
            t = py / CAP_H
            # Solid body, catching a little more light along the top —
            # but NOT up at full: with one tint you cannot be brighter
            # than the tint, so the index line only stands out if the
            # body leaves it some headroom.
            v = 0.66 + 0.14 * max(0.0, 1.0 - abs(t - 0.22) * 3.2)
            for cy, half in CAP_GROOVES:
                near = max(0.0, 1.0 - abs(t - cy) / half)
                if near > 0.0:
                    v = min(v, 1.0 - 0.72 * min(1.0, near * 1.9))
            near = max(0.0, 1.0 - abs(t - CAP_INDEX[0]) / CAP_INDEX[1])
            if near > 0.0:
                v = max(v, min(1.0, near * 2.4))
            # the moulding falls away at the left and right shoulders
            shoulder = min(1.0, (CAP_W / 2.0 - abs(px - CAP_W / 2.0)) / 3.5)
            v *= 0.62 + 0.38 * shoulder
            out.append(clamp(a * v * 255))
    return out


def slim_track():
    """The compact slider's bar: a dark rounded capsule, 9-patched along
    its length so the ends stay round at any size.

    The full-size track is a wide moulding with a groove cut down the
    middle of it. At 8px there is nothing to cut into: the bar IS the
    groove, so this is the groove's colour with a lit rim around it."""
    out = []
    for y in range(SLIM_TRACK_H):
        for x in range(SLIM_TRACK_W):
            d, a = rounded_alpha(x + 0.5, y + 0.5, SLIM_TRACK_W, SLIM_TRACK_H,
                                 SLIM_TRACK_W / 2.0)
            if a == 0.0:
                out.append(0)
                continue
            col = (30, 33, 40)
            if d > -1.4:
                col = mix(col, (76, 82, 96), min(1.0, (d + 1.4) / 1.4))
            out.append((clamp(a * 255) << 24) | (col[0] << 16) |
                       (col[1] << 8) | col[2])
    return out


def slim_cap():
    """The compact handle: a plain rounded rectangle and nothing else.

    No grooves and no index line — both are mouldings you read at 56px
    and mud at 14, and the handle's own EDGES are the index line here,
    since it overhangs the bar it rides on by 8px each side.

    Flat, and FULLY OPAQUE, which is the opposite of what the full-size
    cap does and is forced by what it sits on. In A8, alpha is the only
    variable there is: shading a body means making it see-through, and
    what shows through a handle this small is the bar directly under it —
    two vertical seams down the middle of the block, which reads as a
    lozenge of glass rather than as a handle. The full cap gets away with
    it because its moulding is 30px of an even 48px track; here opacity
    wins and the shape carries it."""
    out = []
    for y in range(SLIM_CAP_H):
        for x in range(SLIM_CAP_W):
            _d, a = rounded_alpha(x + 0.5, y + 0.5,
                                  SLIM_CAP_W, SLIM_CAP_H, 4)
            out.append(clamp(a * 255))
    return out


CHECK = 28

# scrollbar: a 6px capsule, 9-patched to any length (ends stay round)
SBAR_W = 6
SBAR_H = 9
SBAR_INSET = 4
PANEL = 24
PANEL_INSET = 8
ARROW_W, ARROW_H = 12, 8


def sbar(w, h, radius, col, alpha):
    """A scrollbar piece: a rounded capsule, stretched vertically by the
    9-patch. Two of these make the widget — a faint track and a solid
    thumb — so both keep rounded ends at any length."""
    out = []
    for y in range(h):
        for x in range(w):
            _d, a = rounded_alpha(x + 0.5, y + 0.5, w, h, radius)
            out.append((clamp(a * alpha) << 24) | (col[0] << 16) |
                       (col[1] << 8) | col[2])
    return out


RADIO = 24


def radio_strip():
    """Two frames of a radio button: an empty ring, and a ring with a
    dot. ARGB like the checkbox beside it rather than A8, because the
    same two greys and the same accent have to read the same way — a
    radio and a checkbox on one panel that disagree about their own
    colours look like two libraries."""
    out = [0] * (RADIO * 2 * RADIO)
    c = RADIO / 2.0
    for frame in range(2):
        for y in range(RADIO):
            for x in range(RADIO):
                px, py = x + 0.5, y + 0.5
                r = math.hypot(px - c, py - c)
                a = max(0.0, min(1.0, (c - 1.0 - r) * 1.6 + 0.5))
                if a == 0.0:
                    continue
                col = (44, 47, 56)
                if r > c - 3.2:            # the rim, the checkbox's grey
                    col = (110, 118, 134)
                if frame == 1 and r < c * 0.42:
                    col = (110, 205, 160)
                out[y * RADIO * 2 + frame * RADIO + x] = \
                    (clamp(a * 255) << 24) | (col[0] << 16) | (col[1] << 8) | col[2]
    return out


def checkbox_strip():
    out = [0] * (CHECK * 2 * CHECK)
    for frame in range(2):
        for y in range(CHECK):
            for x in range(CHECK):
                px, py = x + 0.5, y + 0.5
                d, a = rounded_alpha(px, py, CHECK, CHECK, 6)
                if a == 0.0:
                    continue
                col = (44, 47, 56)
                if d > -2.2:
                    col = (110, 118, 134)
                if frame == 1:
                    dd = min(seg_dist(px, py, 7, 14.5, 12, 19.5),
                             seg_dist(px, py, 12, 19.5, 21, 8.5))
                    col = mix(col, (110, 205, 160), max(0.0, min(1.0, 2.6 - dd)))
                out[y * CHECK * 2 + frame * CHECK + x] = \
                    (clamp(a * 255) << 24) | (col[0] << 16) | (col[1] << 8) | col[2]
    return out


def panel():
    out = []
    for y in range(PANEL):
        for x in range(PANEL):
            px, py = x + 0.5, y + 0.5
            d, a = rounded_alpha(px, py, PANEL, PANEL, 6)
            if a == 0.0:
                out.append(0)
                continue
            col = (47, 51, 62)
            if d > -1.6:
                col = (96, 103, 120)
            out.append((clamp(a * 255) << 24) | (col[0] << 16) | (col[1] << 8) | col[2])
    return out


def arrow_strip():
    out = [0] * (ARROW_W * 2 * ARROW_H)
    for frame in range(2):
        for y in range(ARROW_H):
            for x in range(ARROW_W):
                px, py = x + 0.5, y + 0.5
                if frame == 1:
                    py = ARROW_H - py  # open state points up
                # triangle: width tapers to a point at the bottom
                half = (ARROW_W / 2.0) * (1.0 - py / ARROW_H)
                cov = max(0.0, min(1.0, half - abs(px - ARROW_W / 2.0) + 0.5))
                if cov > 0.0:
                    out[y * ARROW_W * 2 + frame * ARROW_W + x] = \
                        (clamp(cov * 255) << 24) | (186 << 16) | (192 << 8) | 204
    return out


# ---- LED and selector knob (added for the TB-303 panel) ----

LED = 16          # lens+halo box; the lens itself is much smaller
LED_FRAMES = 6    # off .. full, so a blink can also fade


def led_strip():
    """An indicator lamp, A8 so ONE asset is any color: each LED owns a
    copy of the surf_image with its own tint, which on the P4 is a single
    palette register the PPA applies at blend time.

    Alpha carries the whole look. Frame 0 is the UNLIT lens - not blank,
    because a dead LED is a visible dark bead, not a hole - and later
    frames add a core and a halo. The specular dot stays put across
    frames so the lens reads as glass, not as a disc that grows."""
    c = LED / 2.0
    lens_r = LED * 0.28
    out = []
    for f in range(LED_FRAMES):
        t = f / (LED_FRAMES - 1)
        for y in range(LED):
            for x in range(LED):
                px, py = x + 0.5, y + 0.5
                d = math.hypot(px - c, py - c)
                edge = max(0.0, min(1.0, (lens_r - d) * 1.6 + 0.5))
                # 0.55 unlit, not 0.2: these sit on white piano keys as
                # well as black panels, and a faint tint over white reads
                # as pink rather than as a dead lamp
                a = edge * (0.55 + 0.45 * t)
                if d > lens_r:      # halo, only when lit
                    glow = math.exp(-(d - lens_r) / (LED * 0.14))
                    a = max(a, glow * t * 0.55)
                hl = math.hypot(px - (c - lens_r * 0.32), py - (c - lens_r * 0.34))
                a = max(a, max(0.0, 1.0 - hl / (lens_r * 0.42)) ** 2 *
                            (0.45 + 0.4 * t))
                out.append(clamp(a * 255))
    px = [0] * (LED * LED_FRAMES * LED)
    i = 0
    for f in range(LED_FRAMES):
        for y in range(LED):
            for x in range(LED):
                px[y * LED * LED_FRAMES + f * LED + x] = out[i]
                i += 1
    return px


SEL = 56          # selector knob: the big ones on a 303's middle row


def selector_strip(size=SEL):
    """A light chrome knob with a hard-edged wedge, for the detented
    selector. Same 64-frame filmstrip as the knob - the widget just picks
    the frame nearest a detent - but bright, because these sit on a pale
    panel, and with a WEDGE rather than a hairline: at a detent you want
    to read the position from across the room."""
    c = size / 2.0
    body_r = c - 2.0
    out = [0] * (size * KNOB_FRAMES * size)
    for f in range(KNOB_FRAMES):
        ang = math.radians(-135.0 + 270.0 * f / (KNOB_FRAMES - 1))
        ca, sa = math.cos(-ang), math.sin(-ang)
        for y in range(size):
            for x in range(size):
                px, py = x + 0.5, y + 0.5
                dx, dy = px - c, py - c
                r = math.hypot(dx, dy)
                a = max(0.0, min(1.0, (body_r - r) * 1.6 + 0.5))
                if a == 0.0:
                    continue
                sh = 0.72 + 0.42 * (-(dx + dy) / (2.0 * body_r) + 0.5)
                col = tuple(clamp(v * sh) for v in (196, 198, 206))
                if r > body_r - 2.0:
                    col = tuple(clamp(v * 0.55) for v in col)
                wx, wy = dx * ca - dy * sa, dx * sa + dy * ca
                # one bright wedge, widening toward the rim, and a dark
                # spindle at the middle: two tones inside the wedge read
                # as a chip out of the knob rather than a pointer
                if wy < 0 and abs(wx) < (-wy) * 0.24 + 1.0 and r < body_r - 2.5:
                    col = (252, 252, 254)
                if r < body_r * 0.17:
                    col = tuple(clamp(v * 0.42) for v in (196, 198, 206))
                out[y * size * KNOB_FRAMES + f * size + x] = ink(col, a)
    return out


def transpose(px, w, h):
    """Rotate a WxH pixel list a quarter turn. The slider's groove and its
    cap are drawn for a vertical control; a horizontal one needs the same
    art lying down, and a 9-patch cannot get there by stretching — same
    lesson as the scrollbar's beads."""
    out = [0] * (w * h)
    for y in range(h):
        for x in range(w):
            out[x * h + (h - 1 - y)] = px[y * w + x]
    return out


def emit8(name, values, per_line=24):
    """A8 art is one BYTE per pixel - emitting it as uint32 would quadruple
    a strip that is already 1.5 KB and lie about the stride."""
    print(f"static const uint8_t {name}[{len(values)}] = {{")
    for i in range(0, len(values), per_line):
        print("    " + ", ".join(str(v) for v in values[i:i + per_line]) + ",")
    print("};")


def emit(name, values, per_line=12):
    # const → flash .rodata on device; RAM can't hold the big strips
    print(f"static const uint32_t {name}[{len(values)}] = {{")
    for i in range(0, len(values), per_line):
        print("    " + ", ".join(f"0x{v:x}" for v in values[i:i + per_line]) + ",")
    print("};")


def ref():
    """`--ref`: the three strips src/widgets/art.c bakes at runtime, as C
    arrays, for the TEST build alone (build/gen/widget_art_ref.h) -- so
    test_art_strips can hold the C port to this file byte for byte. Never
    included by anything that ships; that is the whole point of art.c."""
    print("/* Generated by tools/gen_widget_assets.py --ref -- the reference")
    print("   strips for test_art_strips. Test build only. */")
    emit8("ref_knob_px", knob_strip())
    emit8("ref_knobsm_px", knob_strip(KNOB_SM))
    emit8("ref_sel_px", selector_strip())


def main():
    if len(sys.argv) > 1 and sys.argv[1] == "--ref":
        ref()
        return
    print("/* Generated by tools/gen_widget_assets.py — do not edit. */")
    print(f"#define WKNOB_SIZE {KNOB}")
    print(f"#define WKNOB_FRAMES {KNOB_FRAMES}")
    print(f"#define WKNOB_STRIP_W {KNOB * KNOB_FRAMES}")
    print(f"#define WTRACK_SIZE {TRACK}")
    print(f"#define WTRACK_INSET {TRACK_INSET}")
    print(f"#define WTRACKFULL_W {TRACKFULL_W}")
    print(f"#define WTRACKFULL_H {TRACKFULL_H}")
    print(f"#define WCAP_W {CAP_W}")
    print(f"#define WCAP_H {CAP_H}")
    print(f"#define WSLIMTRACK_W {SLIM_TRACK_W}")
    print(f"#define WSLIMTRACK_H {SLIM_TRACK_H}")
    print(f"#define WSLIMTRACK_INSET {SLIM_TRACK_INSET}")
    print(f"#define WSLIMCAP_W {SLIM_CAP_W}")
    print(f"#define WSLIMCAP_H {SLIM_CAP_H}")
    print(f"#define WCHECK_SIZE {CHECK}")
    print(f"#define WRADIO_SIZE {RADIO}")
    print(f"#define WPANEL_SIZE {PANEL}")
    print(f"#define WPANEL_INSET {PANEL_INSET}")
    print(f"#define WARROW_W {ARROW_W}")
    print(f"#define WARROW_H {ARROW_H}")
    print(f"#define WKNOBSM_SIZE {KNOB_SM}")
    print(f"#define WKNOBSM_STRIP_W {KNOB_SM * KNOB_FRAMES}")
    print(f"#define WSBAR_W {SBAR_W}")
    print(f"#define WSBAR_H {SBAR_H}")
    print(f"#define WSBAR_INSET {SBAR_INSET}")
    print(f"#define WLED_SIZE {LED}")
    print(f"#define WLED_FRAMES {LED_FRAMES}")
    print(f"#define WSEL_SIZE {SEL}")
    print("#define WBTN_SIZE 18")
    print("#define WBTN_INSET 6")
    print(f"#define WTAB_W {TAB_W}")
    print(f"#define WTAB_H {TAB_H}")
    print(f"#define WTAB_INSET_TOP {TAB_INSET_TOP}")
    print(f"#define WTAB_INSET_BOT {TAB_INSET_BOT}")
    print(f"#define WTAB_INSET_SIDE {TAB_R + 2}")
    emit("widget_radio_px", radio_strip())
    emit8("widget_tab_px", tab_patch())
    # The knob and selector strips are NOT emitted: at 565 KB between them
    # they were most of this header and a tenth of the device image, and
    # the P4 was copying them flash->PSRAM at init anyway. src/widgets/art.c
    # bakes them on first use, from a line-for-line port of knob_strip()
    # and selector_strip() ABOVE -- which stay here as the reference the C
    # is checked against, byte for byte, by test_art_strips through the
    # `--ref` output below. Change the look here first, then mirror it.
    emit("widget_btn_px", button_patch(False))
    emit("widget_btnpr_px", button_patch(True))
    emit("widget_track_px", track(TRACK, TRACK))
    emit("widget_trackfull_px", track(TRACKFULL_W, TRACKFULL_H))
    emit8("widget_cap_px", cap())
    # ...and the same two lying down, for a horizontal slider
    emit("widget_trackh_px", transpose(track(TRACK, TRACK), TRACK, TRACK))
    emit8("widget_caph_px", transpose(cap(), CAP_W, CAP_H))
    # the compact pair, and the same two lying down
    emit("widget_slimtrack_px", slim_track())
    emit8("widget_slimcap_px", slim_cap())
    emit("widget_slimtrackh_px",
         transpose(slim_track(), SLIM_TRACK_W, SLIM_TRACK_H))
    emit8("widget_slimcaph_px", transpose(slim_cap(), SLIM_CAP_W, SLIM_CAP_H))
    emit("widget_check_px", checkbox_strip())
    emit("widget_panel_px", panel())
    emit("widget_arrow_px", arrow_strip())
    emit8("widget_led_px", led_strip())
    emit("widget_sbar_px", sbar(SBAR_W, SBAR_H, SBAR_W / 2.0, (150, 150, 158), 255))
    emit("widget_sbtrack_px", sbar(SBAR_W, SBAR_H, SBAR_W / 2.0, (255, 255, 255), 40))
    # ...and the same capsules lying down. A 9-patch slices along fixed
    # axes, so a horizontal bar cannot reuse the vertical art: stretching
    # it sideways tiles the round CAP and the thumb comes out as a string
    # of beads. The thickness (and so the radius) is unchanged.
    emit("widget_sbarh_px", sbar(SBAR_H, SBAR_W, SBAR_W / 2.0, (150, 150, 158), 255))
    emit("widget_sbtrackh_px", sbar(SBAR_H, SBAR_W, SBAR_W / 2.0, (255, 255, 255), 40))


if __name__ == "__main__":
    main()
