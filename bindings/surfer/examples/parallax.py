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
    XMIN, XMAX = 8, W * 2 // 3 - ship.w   # nose stays left of the 2/3 line
    YMIN, YMAX = 16, SKY_H + MT_H - ship.h + 24
    pos = [180.0, float(SKY_H + MT_H - ship.h - 40)]
    vel = [0.0, 0.0]
    ACC, VMAX, DRAG = 1.1, 11.0, 0.86     # px/f^2, px/f, per-frame decay
    ship.y_pos = int(pos[1])

    # laser pool: 6 circle shots, each an A8 mask with its own cycling
    # phase (purple -> pink -> green). Tiny masks: shots ride the fast
    # bands, and a moving overlay costs its bbox every frame.
    shots = []
    for i in range(6):
        m = surfer.image_new(16, 16, surfer.A8)
        m.circle(8, 8, 5, (0xffff, 255))
        m.circle(8, 8, 7, (0xffff, 90), 2)    # soft outer glow ring
        sp = surfer.sprite(m, 0, 0)
        sp.hidden = True
        scene.add(sp)
        shots.append({"s": sp, "m": m, "x": 0, "y": 0, "live": False})
    cool = [0]

    LASER = ((172, 64, 255), (255, 96, 208), (96, 255, 128))

    def laser_color(t):
        i = int(t * 3) % 3
        a, b = LASER[i], LASER[(i + 1) % 3]
        u = t * 3 - int(t * 3)
        return surfer.rgb(int(a[0] + (b[0] - a[0]) * u),
                          int(a[1] + (b[1] - a[1]) * u),
                          int(a[2] + (b[2] - a[2]) * u))

    try:
        import parallax_auto        # optional test autopilot: held(frame)
        auto_held = parallax_auto.held
    except ImportError:
        auto_held = None

    # on-screen fps meter (top-left, over the slow sky band; a static
    # overlay on a fast band costs its small bbox per frame)
    meter = surfer.label("-- fps", 12, 8, surfer.rgb(255, 255, 160))
    scene.add(meter)

    state = {"frames": 0, "t0": time.ticks_ms(), "n": 0}

    def _step():
        f = state["frames"] = state["frames"] + 1

        # held-state flight model (keys_held, not key events): thrust
        # while held, drag when released — momentum, not a grid. keys()
        # still drains the event queue so nothing leaks to the REPL.
        surfer.keys()
        if cool[0] > 0:
            cool[0] -= 1
        ax = ay = 0
        firing = False
        for kind, text in (auto_held(f) if auto_held else surfer.keys_held()):
            if kind == surfer.KEY_LEFT:
                ax -= 1
            elif kind == surfer.KEY_RIGHT:
                ax += 1
            elif kind == surfer.KEY_UP:
                ay -= 1
            elif kind == surfer.KEY_DOWN:
                ay += 1
            elif kind == surfer.KEY_TEXT and text == " ":
                firing = True
        vel[0] = (vel[0] + ax * ACC) if ax else vel[0] * DRAG
        vel[1] = (vel[1] + ay * ACC) if ay else vel[1] * DRAG
        vel[0] = min(max(vel[0], -VMAX), VMAX)
        vel[1] = min(max(vel[1], -VMAX), VMAX)
        pos[0] += vel[0]
        pos[1] += vel[1]
        for i, lo, hi in ((0, XMIN, XMAX), (1, YMIN, YMAX)):
            if pos[i] < lo:
                pos[i], vel[i] = lo, 0.0
            elif pos[i] > hi:
                pos[i], vel[i] = hi, 0.0
        if firing and cool[0] == 0:
            for sh in shots:
                if not sh["live"]:
                    sh["live"] = True
                    sh["x"] = int(pos[0]) + ship.w
                    sh["y"] = int(pos[1]) + ship.h // 2 - 8
                    sh["s"].hidden = False
                    cool[0] = 8
                    break

        # forward position sets the pace: 0.5x at the left edge up to
        # 2x at the 2/3 line — pushing ahead feels like accelerating
        boost = 0.5 + 1.5 * (pos[0] - XMIN) / (XMAX - XMIN)
        for i, l in enumerate(layers):
            offs[i] = (offs[i] + speeds[i] * boost) % SW
            l.set_offset(offs[i])

        # overlays move only AFTER the layers shift: a fast layer heals
        # the smear at each overlay's position as of set_offset time, so
        # shift-then-move keeps old ghosts inside the healed region
        ship.x_pos = int(pos[0])
        ship.y_pos = int(pos[1])

        for k, sh in enumerate(shots):
            if not sh["live"]:
                continue
            sh["x"] += 16
            if sh["x"] > W:
                sh["live"] = False
                sh["s"].hidden = True
                continue
            sh["m"].tint = laser_color((f * 0.02 + k / 3.0) % 1.0)
            sh["s"].x_pos = sh["x"]   # the move damages; tint rides along
            sh["s"].y_pos = sh["y"]

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

        state["n"] += 1
        now = time.ticks_ms()
        dt = time.ticks_diff(now, state["t0"])
        if dt >= 1000:
            fps = state["n"] * 1000.0 / dt
            print("parallax: %.1f fps (%.2f ms/frame)" % (fps, dt / state["n"]))
            meter.set_text("%d fps" % int(fps + 0.5))
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
