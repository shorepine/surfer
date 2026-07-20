# Forest walk: an elf (arrow keys / touch-drag) in a dense scrolling
# forest, penned in by a ring of rocks, with slime-critters wandering on
# their own. Logs, rocks and tree trunks block everyone. The world is
# 2 screens x 2 screens (2048x1200); the camera follows the elf.
#
# Rendering: the ENTIRE static world — floor, boulder ring, bushes,
# fallen logs (dead trees baked at rot=90 by image_blit's rot arg),
# trees — is composited ONCE at load into a single 2048x1200 opaque
# image (surfer.image_new + blit). The camera is a full-screen sprite
# whose src rect pans over it with fast_scroll(True): per frame that is
# one DMA band shift + sliver repaints instead of recomposing ~200
# nodes (which ran ~20 fps on the P4). Elf and critters are ordinary
# sprites layered after the camera; it damages them as it pans.
#
# Art from the Kenney CC0 library, frozen into forest_assets.py for
# P4/web. Run from tulip mode: import forest
import time

import surfer

W, H = 1024, 600
WW, WH = 2048, 1200         # world: 2 screens x 2 screens
CX, CY = WW // 2, WH // 2
RX, RY = 940, 540           # the rock ring (ellipse radii)

LIB = "assets/kenney/lib/"

try:
    import forest_assets as _FA   # frozen bytes (P4/web builds)
except ImportError:
    _FA = None


def img(path):
    if _FA is not None:
        name = path.rsplit("/", 1)[-1][:-4].upper()
        return surfer.image(getattr(_FA, name))
    with open(LIB + path, "rb") as f:
        return surfer.image(f.read())


def main():
    root = surfer.screen()
    scene = surfer.group(0, 0)
    root.add(scene)

    # ---- deterministic layout rng
    seed = [1234567]

    def rnd(n):
        seed[0] = (seed[0] * 1103515245 + 12345) & 0x7FFFFFFF
        return seed[0] % n

    def inside_ring(x, y, margin=60):
        dx, dy = x - CX, y - CY
        rx, ry = RX - margin, RY - margin
        return (dx * dx) / (rx * rx) + (dy * dy) / (ry * ry) <= 1.0

    obstacles = []  # (x, y, w, h) in world coords, blocks walkers

    def blocked(x, y, w, h):
        if not inside_ring(x + w // 2, y + h):  # feet point
            return True
        for ox, oy, ow, oh in obstacles:
            if x < ox + ow and x + w > ox and y < oy + oh and y + h > oy:
                return True
        return False

    # ---- bake the whole static world into one opaque image
    print("forest: baking the world...")
    world = surfer.image_new(WW, WH)
    world.fill(surfer.rgb(52, 74, 44))

    import math
    boulder = img("2d/New Platformer Pack/Sprites/Tiles/rock.png")
    for i in range(48):
        a = i * 2 * math.pi / 48
        world.blit(boulder, int(CX + RX * math.cos(a)) - 32,
                   int(CY + RY * math.sin(a)) - 40)

    bush = img("2d/Background Elements Remastered/Retina/bush2.png")
    for i in range(30):
        x, y = rnd(WW - 140), rnd(WH - 140)
        if inside_ring(x, y, 120):
            world.blit(bush, x, y)
    bush.destroy()

    # fallen logs: dead trees baked on their side
    dead = img("2d/Background Elements Remastered/Retina/treeDead.png")
    for i in range(4):
        x, y = 220 + rnd(WW - 800), 160 + rnd(WH - 600)
        if not inside_ring(x, y, 200):
            continue
        if x < CX + 200 and x + dead.h > CX - 200 and \
                y < CY + 200 and y + dead.w > CY - 200:
            continue  # keep the spawn clearing open
        world.blit(dead, x, y, 90 if i % 2 == 0 else 270)
        obstacles.append((x + 30, y + 130, dead.h - 60, 90))
    dead.destroy()

    for i in range(10):
        x, y = 150 + rnd(WW - 400), 120 + rnd(WH - 300)
        if not inside_ring(x, y, 140):
            continue
        world.blit(boulder, x, y)
        obstacles.append((x + 6, y + 20, 52, 40))
    boulder.destroy()

    # the dense forest: trunks block, canopies don't
    tree_a = img("2d/Block Pack/foliageTree_green.png")
    tree_b = img("2d/Block Pack/foliageTree_orange.png")
    for i in range(96):
        x, y = 80 + rnd(WW - 220), 60 + rnd(WH - 260)
        if not inside_ring(x, y, 110):
            continue
        if abs(x - CX) < 150 and abs(y - CY) < 150:
            continue  # keep the spawn clearing open
        world.blit(tree_a if rnd(4) else tree_b, x, y)
        obstacles.append((x + 18, y + 76, 28, 22))
    tree_a.destroy()
    tree_b.destroy()

    pine = img("2d/Background Elements Remastered/Retina/treePine.png")
    for i in range(3):
        x, y = 150 + rnd(WW - 700), 60 + rnd(WH - 800)
        if not inside_ring(x, y, 200):
            continue
        if abs(x - CX) < 260 and abs(y - CY) < 320:
            continue
        world.blit(pine, x, y)
        obstacles.append((x + 76, y + 440, 60, 50))
    pine.destroy()

    # ---- camera: a screen-sized window panned over the world
    cam = surfer.sprite(world, 0, 0)
    cam.set_src(CX - W // 2, CY - H // 2, W, H)
    cam.fast_scroll(True)
    scene.add(cam)

    # ---- critters: slime walkers, 2 frames each (toggle .hidden)
    slime_imgs = [(img("2d/New Platformer Pack/Sprites/Enemies/slime_%s_walk_a.png" % k),
                   img("2d/New Platformer Pack/Sprites/Enemies/slime_%s_walk_b.png" % k))
                  for k in ("normal", "fire", "spike")]
    critters = []
    for i in range(6):
        while True:
            x, y = 200 + rnd(WW - 500), 200 + rnd(WH - 500)
            if inside_ring(x, y, 140) and not blocked(x, y, 64, 64):
                break
        a_img, b_img = slime_imgs[i % len(slime_imgs)]
        sa = surfer.sprite(a_img, -200, -200)
        sb = surfer.sprite(b_img, -200, -200)
        sb.hidden = True
        scene.add(sa)
        scene.add(sb)
        critters.append({"a": sa, "b": sb, "x": x, "y": y,
                         "dx": (1, -1, 1, -1, 2, -2)[i], "dy": (1, 1, -1, -1, 1, -1)[i]})

    # ---- the elf (1:1 art keeps it on the cheap identity path)
    ex, ey = CX - 40, CY - 55
    elf_w1 = img("2d/Platformer Characters 1/Adventurer/Poses/adventurer_walk1.png")
    elf_w2 = img("2d/Platformer Characters 1/Adventurer/Poses/adventurer_walk2.png")
    elf_a = surfer.sprite(elf_w1, -200, -200)
    elf_b = surfer.sprite(elf_w2, -200, -200)
    elf_b.hidden = True
    scene.add(elf_a)
    scene.add(elf_b)
    EW, EH = elf_a.w, elf_a.h

    state = {"frames": 0, "step": 0, "moving": 0, "facing": 1,
             "t0": time.ticks_ms(), "n": 0}
    campos = [0, 0]

    def place():
        cx = min(max(ex - W // 2, 0), WW - W)
        cy = min(max(ey - H // 2, 0), WH - H)
        campos[0], campos[1] = cx, cy
        cam.set_src(cx, cy, W, H)   # every frame: pan or heal (hal contract)
        show_b = state["moving"] > 0 and (state["step"] // 4) % 2 == 1
        for e, hide in ((elf_a, show_b), (elf_b, not show_b)):
            e.x_pos = ex - cx
            e.y_pos = ey - cy
            e.mirror_x = state["facing"] < 0
            e.hidden = hide

    def try_move(dx, dy):
        nonlocal ex, ey
        if dx > 0:
            state["facing"] = 1
        elif dx < 0:
            state["facing"] = -1
        nx, ny = ex + dx, ey + dy
        if not blocked(nx + 12, ny + EH - 20, EW - 24, 18):
            ex, ey = nx, ny
            return True
        if dx and not blocked(ex + 12, ny + EH - 20, EW - 24, 18):
            ey = ny
            return True
        if dy and not blocked(nx + 12, ey + EH - 20, EW - 24, 18):
            ex = nx
            return True
        return False

    # tap (or hold) anywhere: the elf walks toward that spot
    target = [None]

    def on_touch(phase, x, y):
        if phase == surfer.TOUCH_UP:
            return
        target[0] = (x + campos[0] - EW // 2,
                     y + campos[1] - EH + 10)
    # on the scene group: every child (camera, critters, elf) routes here
    scene.on_touch = on_touch

    import builtins
    auto = getattr(builtins, "FOREST_AUTOWALK", False)
    auto_dir = [3, 0]

    def _step():
        f = state["frames"] = state["frames"] + 1

        moved = False
        for kind, text, shift in surfer.keys():
            if kind == surfer.KEY_LEFT:
                moved = try_move(-14, 0) or moved
            elif kind == surfer.KEY_RIGHT:
                moved = try_move(14, 0) or moved
            elif kind == surfer.KEY_UP:
                moved = try_move(0, -14) or moved
            elif kind == surfer.KEY_DOWN:
                moved = try_move(0, 14) or moved
        if target[0] is not None:
            tx, ty = target[0]
            dx = 0 if abs(tx - ex) < 6 else (6 if tx > ex else -6)
            dy = 0 if abs(ty - ey) < 6 else (6 if ty > ey else -6)
            if dx == 0 and dy == 0:
                target[0] = None
            elif try_move(dx, dy):
                moved = True
            else:
                target[0] = None  # blocked both ways: give up
        if auto:
            if not try_move(auto_dir[0], auto_dir[1]) or rnd(90) == 0:
                auto_dir[0], auto_dir[1] = (
                    (3, 0), (-3, 0), (0, 3), (0, -3), (3, 3), (-3, -3))[rnd(6)]
            moved = True
        if moved:
            state["moving"] = 8
            state["step"] += 1
        elif state["moving"] > 0:
            state["moving"] -= 1
        place()

        # critters wander; bounce off anything they can't cross
        for i, c in enumerate(critters):
            if f % 2 == 0:
                nx, ny = c["x"] + c["dx"], c["y"] + c["dy"]
                if blocked(nx + 8, ny + 40, 48, 22):
                    if rnd(2):
                        c["dx"] = -c["dx"]
                    else:
                        c["dy"] = -c["dy"]
                    if rnd(3) == 0:
                        c["dx"], c["dy"] = c["dy"], c["dx"]
                else:
                    c["x"], c["y"] = nx, ny
            walk_b = (f // 16 + i) % 2 == 0
            for which, node in (("a", c["a"]), ("b", c["b"])):
                node.x_pos = c["x"] - campos[0]
                node.y_pos = c["y"] - campos[1]
                node.hidden = (which == "b") != walk_b

        state["n"] += 1
        now = time.ticks_ms()
        dt = time.ticks_diff(now, state["t0"])
        if dt >= 2000:
            print("forest: %.1f fps (%.2f ms/frame)" %
                  (state["n"] * 1000.0 / dt, dt / state["n"]))
            state["t0"] = now
            state["n"] = 0
        return True

    place()

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
        if shot and state["frames"] == 90:
            surfer.screenshot(shot)
            return


main()
