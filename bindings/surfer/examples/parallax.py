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
# Run from tulip mode: import parallax   (desktop, P4, or web)
import math
import time

import surfer

W, H = 1024, 600
SW = 2048                     # strip width; % 64 == 0 so tiles seam cleanly
SKY_H = 212                   # disjoint bands: sky / mountains / ground
MT_H = 260
GND_H = 128

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


SKY_BANDS = [(92, 148, 218), (104, 160, 224), (118, 172, 230),
             (134, 184, 236), (152, 196, 242), (172, 208, 246)]


def sky_color(y):
    return surfer.rgb(*SKY_BANDS[min(y // 100, len(SKY_BANDS) - 1)])


def build_sky():
    sky = surfer.image_new(SW, SKY_H)
    for i in range(len(SKY_BANDS)):
        sky.fill(sky_color(i * 100), 0, i * 100, SW, 100)
    for i, name in enumerate(["cloud1", "cloud2", "cloud3", "cloud4",
                              "cloud5", "cloud6"]):
        c = img("2d/Background Elements Remastered/%s.png" % name)
        sky.blit(c, 40 + i * 335, 18 + (i % 3) * 32)
        c.destroy()
    return sky


def build_mountains():
    # opaque strip: the sky gradient continues INSIDE this band (it is
    # horizontally uniform, so it moving at 0.5x is invisible), with the
    # ridge composited over it
    mt = surfer.image_new(SW, MT_H)
    for y in range(0, MT_H, 4):
        mt.fill(sky_color(SKY_H + y), 0, y, SW, 4)
    ridge = img("2d/Background Elements Remastered/Backgrounds/Elements/mountains.png")
    mt.blit(ridge, 0, MT_H - ridge.h)
    mt.blit(ridge, 1024, MT_H - ridge.h)
    ridge.destroy()
    for i, name in enumerate(["mountainA", "mountainB", "mountainC", "mountainB"]):
        m = img("2d/Background Elements Remastered/Backgrounds/Elements/%s.png" % name)
        mt.blit(m, 140 + i * 520, MT_H - m.h + 40)
        m.destroy()
    return mt


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


def main():
    root = surfer.screen()
    scene = surfer.group(0, 0)
    root.add(scene)

    strips = [build_sky(), build_mountains(), build_ground()]
    ys = [0, SKY_H, SKY_H + MT_H]
    speeds = [0.6, 3.0, 6.0]          # 0.1x / 0.5x / 1x
    layers = []
    for strip, y in zip(strips, ys):
        l = surfer.layer(strip, 0, y, W)
        l.fast_scroll(True)
        scene.add(l)
        layers.append(l)
    offs = [0.0, 0.0, 0.0]

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


main()
