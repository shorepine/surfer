# Forest walk: an elf (arrow keys) in a dense scrolling forest, penned in
# by a ring of rocks, with slime-critters wandering on their own. Logs,
# rocks and tree trunks block everyone. The world is 2 screens x 2 screens
# (2048x1200); the camera follows the elf.
#
# Every sprite is loaded at runtime from the Kenney CC0 library by path —
# found by grepping assets/kenney/lib/index.tsv descriptions, which is the
# whole point of the index. Desktop (make run -> import forest); the art
# isn't frozen into web/P4 builds yet.
import time

import surfer

W, H = 1024, 600            # view
WW, WH = 2048, 1200         # world: 2 screens x 2 screens
CX, CY = WW // 2, WH // 2
RX, RY = 940, 540           # the rock ring (ellipse radii)

LIB = "assets/kenney/lib/"


def img(path):
    with open(LIB + path, "rb") as f:
        return surfer.image(f.read())


def main():
    root = surfer.screen()
    world = surfer.group(0, 0)
    root.add(world)

    # ---- art, straight from the index
    tree_a = img("2d/Block Pack/foliageTree_green.png")          # 64x100
    tree_b = img("2d/Block Pack/foliageTree_orange.png")
    pine = img("2d/Background Elements Remastered/Retina/treePine.png")  # 212x508
    bush = img("2d/Background Elements Remastered/Retina/bush2.png")
    dead = img("2d/Background Elements Remastered/Retina/treeDead.png")  # -> logs
    boulder = img("2d/New Platformer Pack/Sprites/Tiles/rock.png")  # 64x64
    elf_w1 = img("2d/Platformer Characters 1/Adventurer/Poses/adventurer_walk1.png")
    elf_w2 = img("2d/Platformer Characters 1/Adventurer/Poses/adventurer_walk2.png")
    slime_imgs = [(img("2d/New Platformer Pack/Sprites/Enemies/slime_%s_walk_a.png" % k),
                   img("2d/New Platformer Pack/Sprites/Enemies/slime_%s_walk_b.png" % k))
                  for k in ("normal", "fire", "spike")]

    # ---- deterministic layout rng
    seed = [1234567]

    def rnd(n):
        seed[0] = (seed[0] * 1103515245 + 12345) & 0x7FFFFFFF
        return seed[0] % n

    def inside_ring(x, y, margin=60):
        dx, dy = x - CX, y - CY
        rx, ry = RX - margin, RY - margin
        return (dx * dx) / (rx * rx) + (dy * dy) / (ry * ry) <= 1.0

    world.add(surfer.rect(0, 0, WW, WH, surfer.rgb(52, 74, 44)))  # forest floor

    obstacles = []  # (x, y, w, h) in world coords, blocks walkers

    def blocked(x, y, w, h):
        if not inside_ring(x + w // 2, y + h):  # feet point
            return True
        for ox, oy, ow, oh in obstacles:
            if x < ox + ow and x + w > ox and y < oy + oh and y + h > oy:
                return True
        return False

    # rock ring (visual for the ellipse fence)
    import math
    for i in range(48):
        a = i * 2 * math.pi / 48
        x = int(CX + RX * math.cos(a)) - 32
        y = int(CY + RY * math.sin(a)) - 40
        world.add(surfer.sprite(boulder, x, y))

    # bushes (walk-through decor)
    for i in range(44):
        x, y = rnd(WW - 120), rnd(WH - 120)
        if inside_ring(x, y, 120):
            b = surfer.sprite(bush, x, y)
            b.scale = 0.5
            world.add(b)

    # fallen logs: dead trees on their side — rot shows the sprite xform
    for i in range(7):
        x, y = 200 + rnd(WW - 500), 150 + rnd(WH - 350)
        if not inside_ring(x, y, 160):
            continue
        lg = surfer.sprite(dead, x, y)
        lg.scale = 0.35
        lg.rot = 90 if i % 2 == 0 else 270
        world.add(lg)
        obstacles.append((x, y + 20, lg.w, lg.h - 30))

    # standalone boulders
    for i in range(10):
        x, y = 150 + rnd(WW - 400), 120 + rnd(WH - 300)
        if not inside_ring(x, y, 140):
            continue
        r = surfer.sprite(boulder, x, y)
        r.scale = 1.3
        world.add(r)
        obstacles.append((x + 6, y + 20, r.w - 12, r.h - 24))

    # the dense forest: trunks block, canopies don't
    for i in range(96):
        x, y = 80 + rnd(WW - 220), 60 + rnd(WH - 260)
        if not inside_ring(x, y, 110):
            continue
        if abs(x - CX) < 140 and abs(y - CY) < 140:
            continue  # keep the spawn clearing open
        t = surfer.sprite(tree_a if rnd(4) else tree_b, x, y)
        world.add(t)
        obstacles.append((x + 18, y + 76, 28, 22))  # trunk base only
    for i in range(7):
        x, y = 120 + rnd(WW - 500), 40 + rnd(WH - 500)
        if not inside_ring(x, y, 150):
            continue
        if abs(x - CX) < 220 and abs(y - CY) < 260:
            continue  # keep the spawn clearing open
        p = surfer.sprite(pine, x, y)
        p.scale = 0.5
        world.add(p)
        obstacles.append((x + 34, y + 214, 38, 30))

    # ---- critters: slime walkers, 2 frames each (toggle .hidden)
    critters = []
    for i in range(6):
        while True:
            x, y = 200 + rnd(WW - 500), 200 + rnd(WH - 500)
            if inside_ring(x, y, 140) and not blocked(x, y, 64, 64):
                break
        a_img, b_img = slime_imgs[i % len(slime_imgs)]
        sa = surfer.sprite(a_img, x, y)
        sb = surfer.sprite(b_img, x, y)
        sb.hidden = True
        world.add(sa)
        world.add(sb)
        critters.append({"a": sa, "b": sb, "x": x, "y": y,
                         "dx": (1, -1, 1, -1, 2, -2)[i], "dy": (1, 1, -1, -1, 1, -1)[i]})

    # ---- the elf
    ex, ey = CX - 24, CY - 40
    elf_a = surfer.sprite(elf_w1, ex, ey)
    elf_b = surfer.sprite(elf_w2, ex, ey)
    for e in (elf_a, elf_b):
        e.scale = 0.6
    elf_b.hidden = True
    world.add(elf_a)
    world.add(elf_b)
    EW, EH = elf_a.w, elf_a.h

    state = {"frames": 0, "step": 0, "moving": 0, "facing": 1}

    def place_elf():
        for e in (elf_a, elf_b):
            e.x_pos = ex
            e.y_pos = ey
            e.mirror_x = state["facing"] < 0  # art faces right
        show_b = state["moving"] > 0 and (state["step"] // 4) % 2 == 1
        elf_a.hidden = show_b
        elf_b.hidden = not show_b
        # camera follows, clamped to the world
        cx = min(max(ex - W // 2, 0), WW - W)
        cy = min(max(ey - H // 2, 0), WH - H)
        world.x_pos = -cx
        world.y_pos = -cy

    place_elf()

    def _step():
        f = state["frames"] = state["frames"] + 1
        nonlocal ex, ey

        moved = False
        for kind, text, shift in surfer.keys():
            dx = dy = 0
            if kind == surfer.KEY_LEFT:
                dx = -14
                state["facing"] = -1
            elif kind == surfer.KEY_RIGHT:
                dx = 14
                state["facing"] = 1
            elif kind == surfer.KEY_UP:
                dy = -14
            elif kind == surfer.KEY_DOWN:
                dy = 14
            else:
                continue
            nx, ny = ex + dx, ey + dy
            # collide on the feet box, slide on the free axis
            if not blocked(nx + 8, ny + EH - 18, EW - 16, 16):
                ex, ey = nx, ny
                moved = True
            elif dx and not blocked(ex + 8, ny + EH - 18, EW - 16, 16):
                ey = ny
                moved = True
            elif dy and not blocked(nx + 8, ey + EH - 18, EW - 16, 16):
                ex = nx
                moved = True
        if moved:
            state["moving"] = 8
            state["step"] += 1
        elif state["moving"] > 0:
            state["moving"] -= 1
        place_elf()

        # critters wander; bounce off anything they can't cross
        for i, c in enumerate(critters):
            if f % 2:
                continue  # half speed
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
                node.x_pos = c["x"]
                node.y_pos = c["y"]
                node.hidden = (which == "b") != walk_b
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
        if shot and state["frames"] == 90:
            surfer.screenshot(shot)
            return


main()
