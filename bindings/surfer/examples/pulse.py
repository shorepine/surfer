# Pulse: Amiga-style color cycling without a palette. Every shape here is
# ONE A8 mask (a PNG's alpha channel, loaded with surfer.image(b, True));
# its color is the image's .tint — a one-entry palette register the P4's
# PPA applies in hardware at blend time. Cycling = write .tint, call
# sprite.damage(). No pixels are ever recomputed on the CPU.
#
# The scene: a glowing spaceship breathing through the spectrum, a ring
# of slimes running a marquee chase around it, and a treeline slowly
# sweeping through sunset hues. Run from repl mode: import pulse
import math
import time

import surfer

W, H = 1024, 600

try:
    import forest_assets as _FA
    import space_assets as _SA
except ImportError:
    _FA = _SA = None

LIB = "assets/kenney/lib/"
_PATHS = {
    "SHIP": "2d/Space Shooter Redux/PNG/playerShip1_blue.png",
    "SLIME_NORMAL_WALK_A": "2d/New Platformer Pack/Sprites/Enemies/slime_normal_walk_a.png",
    "SLIME_FIRE_WALK_A": "2d/New Platformer Pack/Sprites/Enemies/slime_fire_walk_a.png",
    "SLIME_SPIKE_WALK_A": "2d/New Platformer Pack/Sprites/Enemies/slime_spike_walk_a.png",
    "FOLIAGETREE_GREEN": "2d/Block Pack/foliageTree_green.png",
    "ROCK": "2d/New Platformer Pack/Sprites/Tiles/rock.png",
}


def mask(name):
    """Load one asset's alpha channel as a tintable A8 mask."""
    mod = _SA if (_SA and hasattr(_SA, name)) else _FA
    if mod is not None and hasattr(mod, name):
        return surfer.image(getattr(mod, name), True)
    with open(LIB + _PATHS[name], "rb") as f:
        return surfer.image(f.read(), True)


def hsv(h, s, v):
    """h in [0,1) around the wheel -> rgb565."""
    i = int(h * 6.0) % 6
    f = h * 6.0 - int(h * 6.0)
    p, q, t = v * (1 - s), v * (1 - s * f), v * (1 - s * (1 - f))
    r, g, b = ((v, t, p), (q, v, p), (p, v, t),
               (p, q, v), (t, p, v), (v, p, q))[i]
    return surfer.rgb(int(r * 255), int(g * 255), int(b * 255))


def main():
    root = surfer.screen()
    scene = surfer.group(0, 0)
    root.add(scene)
    scene.add(surfer.rect(0, 0, W, H, surfer.rgb(8, 8, 16)))

    # each cycler: (Image, sprite, hue speed, phase, sat, breathe depth)
    cyclers = []

    def add(img, x, y, speed, phase, sat=1.0, breathe=0.0):
        s = surfer.sprite(img, x - img.w // 2, y - img.h // 2)
        scene.add(s)
        cyclers.append((img, s, speed, phase, sat, breathe))
        return s

    # treeline: a slow sunset sweep, each tree a step behind the last
    for i in range(6):
        add(mask("FOLIAGETREE_GREEN"), 90 + i * 170, H - 90,
            0.05, i * 0.07, sat=0.85)

    # rocks as dim embers between the trees
    for i in range(5):
        add(mask("ROCK"), 175 + i * 170, H - 52, 0.05, 0.5 + i * 0.07,
            sat=0.9, breathe=0.35)

    # the marquee: 10 slimes chasing one hue around a ring
    slime = ("SLIME_NORMAL_WALK_A", "SLIME_FIRE_WALK_A", "SLIME_SPIKE_WALK_A")
    for i in range(10):
        a = i * 2 * math.pi / 10
        add(mask(slime[i % 3]),
            int(W // 2 + 300 * math.cos(a)), int(230 + 130 * math.sin(a)),
            0.25, i / 10.0, breathe=0.25)

    # centerpiece: the ship, breathing hard through the full spectrum
    add(mask("SHIP"), W // 2, 230, 0.12, 0.0, breathe=0.55)

    state = {"frames": 0, "t0": time.ticks_ms(), "n": 0}

    def _step():
        state["frames"] += 1
        t = state["frames"] / 60.0
        for img, spr, speed, phase, sat, breathe in cyclers:
            v = 1.0 - breathe * (0.5 + 0.5 * math.sin((t * 1.7 + phase) * 2 * math.pi))
            img.tint = hsv((t * speed + phase) % 1.0, sat, v)
            spr.damage()

        now = time.ticks_ms()
        spent = time.ticks_diff(now, state.get("last", now))
        if 0 <= spent < 16:
            time.sleep_ms(16 - spent)
            now = time.ticks_ms()
        state["last"] = now

        state["n"] += 1
        dt = time.ticks_diff(now, state["t0"])
        if dt >= 2000:
            print("pulse: %.1f fps (%.2f ms/frame)" %
                  (state["n"] * 1000.0 / dt, dt / state["n"]))
            state["t0"] = now
            state["n"] = 0
        return True

    import sys
    if sys.platform == "webassembly":
        import repl
        repl.app_frame = _step
        return

    try:
        import os
        shot = os.getenv("SURF_SHOT")
    except (ImportError, AttributeError):
        shot = None

    while surfer.tick():
        if _step() is False:
            return
        if shot and state["frames"] in (30, 75):
            surfer.screenshot(shot.replace(".", "%d." % state["frames"], 1))
            if state["frames"] == 75:
                return


main()
