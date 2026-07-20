# Controller tester: shows surfer.pad(0) live on screen. Plug in a USB
# gamepad (or use the keyboard map: arrows/WASD + space/ZXCV) and every
# button lights up, both sticks track. Use it to confirm a new driver's
# mapping. Run from tulip mode: import padtest
import time

import surfer

W, H = 1024, 600
OFF = surfer.rgb(40, 46, 60)      # indicator: not pressed
ON = surfer.rgb(90, 230, 130)     # indicator: pressed
INK = surfer.rgb(230, 234, 240)


def main():
    surfer.frame_rate(60)
    root = surfer.screen()
    scene = surfer.group(0, 0)
    root.add(scene)
    scene.add(surfer.rect(0, 0, W, H, surfer.rgb(16, 18, 24)))
    scene.add(surfer.label("surfer.pad(0) tester  -  move the controller", 24, 20, INK))

    pad = surfer.pad(0)

    # button indicators: (attr, label, x, y)
    defs = [("up", "Up", 120, 120), ("down", "Down", 120, 200),
            ("left", "Left", 40, 160), ("right", "Right", 200, 160),
            ("a", "A", 820, 200), ("b", "B", 880, 150),
            ("x", "X", 760, 150), ("y", "Y", 820, 100),
            ("l", "L", 740, 60), ("r", "R", 900, 60),
            ("start", "Start", 560, 500), ("select", "Select", 440, 500)]
    dots = {}
    for attr, lab, x, y in defs:
        r = surfer.rect(x, y, 48, 48, OFF)
        scene.add(r)
        scene.add(surfer.label(lab, x, y + 54, INK))
        dots[attr] = r

    # analog sticks: a box + a dot that tracks (x, y) in [-1, 1]
    def stick(cx, cy, name):
        scene.add(surfer.rect(cx - 90, cy - 90, 180, 180, surfer.rgb(30, 34, 44)))
        scene.add(surfer.label(name, cx - 90, cy + 96, INK))
        d = surfer.rect(cx - 12, cy - 12, 24, 24, surfer.rgb(120, 180, 255))
        scene.add(d)
        return d
    lstick = stick(300, 340, "L stick")
    rstick = stick(560, 340, "R stick")
    lc, rc = (300, 340), (560, 340)

    def _step():
        for attr, r in dots.items():
            r.set_color(ON if getattr(pad, attr) else OFF)
        lstick.x_pos = int(lc[0] + pad.lx * 78) - 12
        lstick.y_pos = int(lc[1] + pad.ly * 78) - 12
        rstick.x_pos = int(rc[0] + pad.rx * 78) - 12
        rstick.y_pos = int(rc[1] + pad.ry * 78) - 12
        return True

    import sys
    if sys.platform == "webassembly":
        import tulip
        tulip.app_frame = _step
        return
    while surfer.tick():
        _step()


main()
