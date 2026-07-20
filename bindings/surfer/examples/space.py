# The sprite demo: runtime PNGs (Kenney "Space Shooter Redux", CC0)
# moving, scaling, and quarter-turn rotating over the live UI. Run from
# tulip mode with `import space`; the compositor repaints whatever the
# sprites uncover — no manual redraw of the background.
#
# Ship follows a touch/drag; meteors tumble and wrap; lasers fire on tap.
import time

import surfer
import space_assets as assets

W, H = 1024, 600

ship_img = surfer.image(assets.SHIP)
enemy_img = surfer.image(assets.ENEMY)
met_big_img = surfer.image(assets.METEOR_BIG)
met_small_img = surfer.image(assets.METEOR_SMALL)
laser_img = surfer.image(assets.LASER)


def main():
    root = surfer.screen()
    layer = surfer.group(0, 0)
    root.add(layer)

    # a dim starfield so motion reads even over empty screen areas
    rng = 12345
    stars = []
    for i in range(24):
        rng = (rng * 1103515245 + 12345) & 0x7FFFFFFF
        x = rng % W
        rng = (rng * 1103515245 + 12345) & 0x7FFFFFFF
        y = rng % H
        s = surfer.rect(x, y, 2, 2, surfer.rgb(90, 95, 110))
        layer.add(s)
        stars.append(s)

    # meteors: mixed sizes, mixed scales, tumbling in quarter turns
    meteors = []
    for i in range(6):
        img = met_big_img if i % 2 == 0 else met_small_img
        m = surfer.sprite(img, 80 + i * 150, 60 + (i % 3) * 140)
        m.scale = 0.5 + (i % 4) * 0.5   # 0.5 .. 2.0
        layer.add(m)
        meteors.append({
            "n": m,
            "vx": 1 + (i % 3),
            "vy": 1 + ((i + 1) % 2),
            "spin": 20 + i * 8,          # frames between quarter turns
        })

    enemy = surfer.sprite(enemy_img, W // 2, 40)
    enemy.scale = 0.75
    layer.add(enemy)

    ship = surfer.sprite(ship_img, W // 2 - 49, H - 100)
    layer.add(ship)

    lasers = []

    def fire(_phase=None, x=None, y=None):
        if len(lasers) >= 8:
            return
        l = surfer.sprite(laser_img, ship.x_pos + ship.w // 2 - 4,
                          ship.y_pos - 40)
        layer.add(l)
        lasers.append(l)

    def on_touch(phase, x, y):
        ship.x_pos = x - ship.w // 2
        if phase == surfer.TOUCH_DOWN:
            fire()
    ship.on_touch = on_touch  # drag the ship; lasers also autofire

    state = {"frames": 0, "dir": 3}

    def _step():
        f = state["frames"] = state["frames"] + 1

        for m in meteors:
            n = m["n"]
            nx, ny = n.x_pos + m["vx"], n.y_pos + m["vy"]
            if nx > W:
                nx = -n.w
            if ny > H:
                ny = -n.h
            n.x_pos = nx
            n.y_pos = ny
            if f % m["spin"] == 0:
                n.rot = (n.rot + 90) % 360

        # enemy strafes and pulses its scale
        ex = enemy.x_pos + state["dir"]
        if ex < 40 or ex > W - 120:
            state["dir"] = -state["dir"]
        enemy.x_pos = ex
        enemy.scale = 0.75 + 0.35 * ((f // 4) % 8) / 8.0

        for l in lasers[:]:
            l.y_pos = l.y_pos - 12
            if l.y_pos < -60:
                l.detach()
                lasers.remove(l)
        if f % 45 == 0:
            fire()
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
        if shot and state["frames"] == 60:  # late enough to catch a laser
            surfer.screenshot(shot)
            return


main()
