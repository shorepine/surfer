#!/usr/bin/env python3
"""Build-time placeholder widget art for M1.

Emits a C header on stdout: a 64-frame knob filmstrip (indicator sweeping
-135°..+135°, matching the knob widget's angular mapping), a 9-patch
slider track, and a slider cap. All straight-alpha ARGB8888. Real themes
come from Blender renders via surfpack later (DESIGN.md §2.4).
"""
import math

KNOB_FRAMES = 64
KNOB = 64
TRACK = 36
TRACK_INSET = 12
TRACKFULL_W, TRACKFULL_H = 48, 330  # baked at the mixer's exact size: 1 blit
CAP_W, CAP_H = 40, 20


def clamp(v, lo=0, hi=255):
    return max(lo, min(hi, int(v)))


def mix(a, b, t):
    return tuple(clamp(x + (y - x) * t) for x, y in zip(a, b))


def seg_dist(px, py, ax, ay, bx, by):
    vx, vy = bx - ax, by - ay
    t = max(0.0, min(1.0, ((px - ax) * vx + (py - ay) * vy) / (vx * vx + vy * vy)))
    return math.hypot(px - (ax + vx * t), py - (ay + vy * t))


def knob_strip():
    c = KNOB / 2.0
    out = []
    frames = []
    for f in range(KNOB_FRAMES):
        ang = math.radians(-135.0 + 270.0 * f / (KNOB_FRAMES - 1))
        dx, dy = math.sin(ang), -math.cos(ang)
        frames.append((c + dx * 8, c + dy * 8, c + dx * 22, c + dy * 22))
    for y in range(KNOB):
        for f in range(KNOB_FRAMES):
            ax, ay, bx, by = frames[f]
            for x in range(KNOB):
                px, py = x + 0.5, y + 0.5
                r = math.hypot(px - c, py - c)
                a = max(0.0, min(1.0, (27.5 - r) * 2.0))
                if a == 0.0:
                    out.append((f, x, y, 0))
                    continue
                if r > 23.0:
                    col = (58, 62, 74)
                else:
                    sh = 1.0 - 0.45 * (r / 23.0) ** 2
                    hr = math.hypot((px - c) / 23.0 + 0.35, (py - c) / 23.0 + 0.35)
                    spec = max(0.0, 1.0 - hr * 1.8) ** 3 * 140.0
                    col = tuple(clamp(v * sh + spec) for v in (126, 130, 142))
                d = seg_dist(px, py, ax, ay, bx, by)
                col = mix(col, (240, 242, 248), max(0.0, min(1.0, 2.7 - d)))
                word = (clamp(a * 255) << 24) | (col[0] << 16) | (col[1] << 8) | col[2]
                out.append((f, x, y, word))
    strip_w = KNOB * KNOB_FRAMES
    px = [0] * (strip_w * KNOB)
    for f, x, y, word in out:
        px[y * strip_w + f * KNOB + x] = word
    return px


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


def cap():
    out = []
    for y in range(CAP_H):
        for x in range(CAP_W):
            px, py = x + 0.5, y + 0.5
            d, a = rounded_alpha(px, py, CAP_W, CAP_H, 7)
            if a == 0.0:
                out.append(0)
                continue
            col = (214, 138, 74)
            if py < 8:
                col = mix(col, (244, 178, 118), (8 - py) / 8.0)
            elif py > 14:
                col = mix(col, (160, 96, 46), (py - 14) / 6.0)
            if d > -1.5:
                col = (150, 92, 48)
            out.append((clamp(a * 255) << 24) | (col[0] << 16) | (col[1] << 8) | col[2])
    return out


CHECK = 28
PANEL = 24
PANEL_INSET = 8
ARROW_W, ARROW_H = 12, 8


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


def emit(name, values, per_line=12):
    # const → flash .rodata on device; RAM can't hold the big strips
    print(f"static const uint32_t {name}[{len(values)}] = {{")
    for i in range(0, len(values), per_line):
        print("    " + ", ".join(f"0x{v:x}" for v in values[i:i + per_line]) + ",")
    print("};")


def main():
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
    print(f"#define WCHECK_SIZE {CHECK}")
    print(f"#define WPANEL_SIZE {PANEL}")
    print(f"#define WPANEL_INSET {PANEL_INSET}")
    print(f"#define WARROW_W {ARROW_W}")
    print(f"#define WARROW_H {ARROW_H}")
    emit("widget_knob_px", knob_strip())
    emit("widget_track_px", track(TRACK, TRACK))
    emit("widget_trackfull_px", track(TRACKFULL_W, TRACKFULL_H))
    emit("widget_cap_px", cap())
    emit("widget_check_px", checkbox_strip())
    emit("widget_panel_px", panel())
    emit("widget_arrow_px", arrow_strip())


if __name__ == "__main__":
    main()
