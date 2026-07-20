# Parallax scrolling on surfer layers: three wrap-scrolling strips at
# 0.1x / 0.5x / 1x with a bobbing ship on top, and a once-a-second fps
# print — the "how fast can surfer scroll full-screen graphics"
# measurement (numbers in DESIGN.md).
#
# The layer recipe: bake tiles into ONE opaque strip per layer at load
# time (surfer.image_new + img.blit), stack surfer.layer() nodes in
# vertically DISJOINT bands, fast_scroll(True) each. Per frame each band
# is one DMA band copy from the last-presented frame plus a sliver
# repaint (the streaming band_shift path) instead of a full recompose —
# 19 fps naive vs full frame rate this way, on the P4. Overlay sprites
# (the ship) must be later siblings of the layers; the layers damage
# them as they shift.
#
# The mountains are PROCEDURAL: gradient-filled triangles from the shape
# API (img.poly with linear-gradient paints), snowcaps included — no
# mountain art anywhere. Three of them are volcanoes: their lava veins
# are bezier strokes baked into A8 masks (coverage-with-falloff), each
# an overlay sprite whose .tint cycles through ember colors per frame —
# Amiga-style color cycling, one hardware blend per volcano.
#
# Run from tulip mode: import parallax   (desktop, P4, or web)
import math
import time

import surfer

W, H = 1024, 600
SW = 2048                     # strip width; % 64 == 0 so tiles seam cleanly
SKY_H = 212                   # disjoint bands: sky / mountains / ground
MT_H = 260
GND_H = 128
ST = SKY_H + MT_H             # the sky gradient spans both bands

LIB = "assets/kenney/lib/"

try:
    import parallax_assets as _PA   # frozen bytes (P4/web builds)
except ImportError:
    _PA = None


def img(path):
    if _PA is not None:
        name = path.rsplit("/", 1)[-1][:-4].upper()
        return surfer.image(getattr(_PA, name))
    with open(LIB + path, "rb") as f:
        return surfer.image(f.read())


SKY_TOP = surfer.rgb(64, 108, 196)
SKY_HOR = surfer.rgb(214, 188, 156)     # warm haze at the horizon


def build_sky():
    sky = surfer.image_new(SW, SKY_H)
    # one smooth vertical gradient; the mountain band continues the same
    # axis so the seam between the two layers is invisible
    sky.poly([(0, 0), (SW, 0), (SW, SKY_H), (0, SKY_H)],
             ((0, 0, SKY_TOP), (0, ST, SKY_HOR)))
    for i, name in enumerate(["cloud1", "cloud2", "cloud3", "cloud4",
                              "cloud5", "cloud6"]):
        c = img("2d/Background Elements Remastered/%s.png" % name)
        sky.blit(c, 40 + i * 335, 18 + (i % 3) * 32)
        c.destroy()
    return sky


def build_mountains():
    """Gradient-triangle ranges; returns (strip, volcano list)."""
    mt = surfer.image_new(SW, MT_H)
    mt.poly([(0, 0), (SW, 0), (SW, MT_H), (0, MT_H)],
            ((0, -SKY_H, SKY_TOP), (0, MT_H, SKY_HOR)))

    # back ridge: hazy blue-grey, fading into the horizon color
    for i in range(7):
        bx = i * 300 - 40
        bw = 380 + (i * 67) % 120
        mh = 130 + (i * 53) % 70
        ax = bx + bw // 2 + (i * 37) % 80 - 40
        mt.poly([(bx, MT_H), (ax, MT_H - mh), (bx + bw, MT_H)],
                ((ax, MT_H - mh, surfer.rgb(150, 158, 186)),
                 (ax, MT_H, surfer.rgb(120, 132, 168))))

    # front ridge: brown gradients lit from the peak; some snowcapped,
    # three of them volcanoes (lava masks made in build_lava)
    volcanoes = []
    front = [(40, 460, 225, 0.42, True), (560, 420, 190, 0.55, False),
             (1080, 480, 235, 0.45, True), (1620, 400, 205, 0.50, True)]
    for i, (bx, bw, mh, apx, lava) in enumerate(front):
        ax = bx + int(bw * apx)
        ay = MT_H - mh
        dark = surfer.rgb(78, 54, 38)
        lit = surfer.rgb(172, 124, 74)
        mt.poly([(bx, MT_H), (ax, ay), (bx + bw, MT_H)],
                ((bx, MT_H, dark), (ax, ay, lit)))
        if lava:
            # crater notch instead of a snowcap
            mt.poly([(ax - 16, ay + 10), (ax, ay), (ax + 16, ay + 10),
                     (ax + 8, ay + 16), (ax - 8, ay + 16)],
                    surfer.rgb(52, 34, 26))
            volcanoes.append((bx, bw, mh, ax - bx))
        else:
            cap = mh // 4
            mt.poly([(ax - int(bw * 0.13), ay + cap), (ax, ay),
                     (ax + int(bw * 0.13), ay + cap),
                     (ax + int(bw * 0.05), ay + cap - 8),
                     (ax - int(bw * 0.06), ay + cap - 4)],
                    ((ax, ay, surfer.rgb(244, 246, 250)),
                     (ax, ay + cap, surfer.rgb(198, 206, 224))))
    return mt, volcanoes


def build_lava(bw, mh, axl):
    """One volcano's lava: bezier veins + crater glow in an A8 mask.
    Coverage fades toward the foot (gradient-alpha paint); the color is
    the mask's .tint, cycled per frame by the caller. The mask is
    cropped to the vein column — these sprites ride a scrolling layer,
    so they re-blend every frame and their bbox is the price."""
    x0 = axl - int(bw * 0.07) - 14
    mw = int(bw * 0.15) + 28
    mhh = int(mh * 0.85) + 4
    ax = axl - x0
    m = surfer.image_new(mw, mhh, surfer.A8)
    fade = ((ax, 6, 0xffff, 255), (ax, mh * 0.85, 0xffff, 0))
    m.bezier([(ax - 2, 8), (ax - 14, mh * 0.3), (ax - 2, mh * 0.5),
              (ax - int(bw * 0.07), mh * 0.82)], fade, 5)
    m.bezier([(ax + 2, 10), (ax + 16, mh * 0.33), (ax + 5, mh * 0.5),
              (ax + int(bw * 0.08), mh * 0.74)], fade, 3.5)
    m.bezier([(ax - 3, 12), (ax - 8, mh * 0.28), (ax - 15, mh * 0.44)],
             fade, 2.5)
    m.ellipse(ax, 8, 9, 4, (0xffff, 255))         # the crater pool
    return m, x0


def main():
    root = surfer.screen()
    scene = surfer.group(0, 0)
    root.add(scene)

    sky = build_sky()
    mt, volcanoes = build_mountains()
    strips = [sky, mt, build_ground()]
    ys = [0, SKY_H, SKY_H + MT_H]
    speeds = [0.6, 3.0, 6.0]          # 0.1x / 0.5x / 1x
    layers = []
    for strip, y in zip(strips, ys):
        l = surfer.layer(strip, 0, y, W)
        l.fast_scroll(True)
        scene.add(l)
        layers.append(l)
    offs = [0.0, 0.0, 0.0]

    # lava overlays ride the mountain layer (later siblings => the layer
    # damages them as it shifts; we retint + damage as they glow)
    lavas = []
    for k, (bx, bw, mh, axl) in enumerate(volcanoes):
        mask, ox = build_lava(bw, mh, axl)
        s = surfer.sprite(mask, -bw, SKY_H + MT_H - mh)
        scene.add(s)
        lavas.append((s, mask, bx + ox, mask.w, k * 2.1))

    ship_img = img("2d/Space Shooter Remastered/playerShip1_blue.png")
    ship = surfer.sprite(ship_img, 180, 0)
    ship.scale = 0.55
    ship.rot = 270          # art points up; face the direction of travel
    scene.add(ship)
    base_y = SKY_H + MT_H - ship.h - 40

    state = {"frames": 0, "t0": time.ticks_ms(), "n": 0}

    def _step():
        f = state["frames"] = state["frames"] + 1
        for i, l in enumerate(layers):
            offs[i] = (offs[i] + speeds[i]) % SW
            l.set_offset(offs[i])

        for s, mask, bx, bw, phase in lavas:
            sx = (bx - offs[1]) % SW
            if sx >= SW - bw:
                sx -= SW            # wrapping in from the left edge
            if sx <= -bw or sx >= W:
                s.hidden = True     # off-screen: no blend, no damage
                continue
            s.hidden = False
            s.x_pos = int(sx)
            glow = 0.5 + 0.5 * math.sin(f * 0.11 + phase)
            mask.tint = surfer.rgb(255, int(60 + 165 * glow), int(34 * glow))
            s.damage()

        ship.y_pos = base_y + int(26 * math.sin(f / 19.0)) + int(8 * math.sin(f / 5.3))

        state["n"] += 1
        now = time.ticks_ms()
        dt = time.ticks_diff(now, state["t0"])
        if dt >= 1000:
            print("parallax: %.1f fps (%.2f ms/frame)" %
                  (state["n"] * 1000.0 / dt, dt / state["n"]))
            state["t0"] = now
            state["n"] = 0
        return True

    import sys
    if sys.platform == "webassembly":
        import tulip
        tulip.app_frame = _step
        return

    try:
        import os
        shot = os.getenv("SURF_SHOT")
    except (ImportError, AttributeError):
        shot = None

    while surfer.tick():
        if _step() is False:
            return
        if shot and state["frames"] == 75:
            surfer.screenshot(shot)
            return


def build_ground():
    g = surfer.image_new(SW, GND_H)
    top = img("2d/New Platformer Pack/Sprites/Tiles/terrain_grass_block_top.png")
    fill = img("2d/New Platformer Pack/Sprites/Tiles/terrain_grass_block.png")
    for x in range(0, SW, 64):
        g.blit(top, x, 0)
        g.blit(fill, x, 64)
    top.destroy()
    fill.destroy()
    decor = ["grass", "bush", "mushroom_red", "fence", "rock", "grass",
             "mushroom_brown", "sign", "grass_purple", "fence_broken"]
    for i, name in enumerate(decor):
        d = img("2d/New Platformer Pack/Sprites/Tiles/%s.png" % name)
        g.blit(d, 40 + i * 200, -d.h + 66)
        d.destroy()
    return g


main()
