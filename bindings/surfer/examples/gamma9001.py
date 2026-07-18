# Gamma 9001, surfer edition — the AMY drum machine's UI rebuilt in
# MicroPython on surfer (UI only; no sound yet). The layout follows the
# web app: 8 channels x 16 steps with per-channel Pitch/Offset/Pan/Vol/
# Cutoff/Res knobs, and the step-seq / pattern / effects / global panels
# living on a second page behind the "controls" button. Channel rows
# scroll vertically; step pads are custom Python widgets built on
# node.on_touch (tap cycles off -> on -> accent; drag scrolls).
#
# Desktop:  ~/micropython/ports/unix/build-standard/micropython \
#               bindings/surfer/examples/gamma9001.py
# Device:   freeze/copy, then `import gamma9001`.
import time
import surfer

W, H = 1024, 600
NUM_CH = 8
NUM_STEPS = 16
NUM_PATTERNS = 16

KIT = ["909 Kick", "909 Snare", "909 Clap", "Conga",
       "909 CH", "909 OH", "Cowbell", "909 Ride"]
CH_KNOBS = ["Pit", "Off", "Pan", "Vol", "Cut", "Res"]
CH_DEFAULTS = [0.5, 0.0, 0.5, 0.66, 1.0, 0.08]

C_BG = surfer.rgb(18, 20, 25)
C_ROW = [surfer.rgb(26, 29, 36), surfer.rgb(30, 33, 41)]
C_TEXT = surfer.rgb(200, 205, 215)
C_DIM = surfer.rgb(140, 146, 158)
C_TITLE = surfer.rgb(240, 242, 248)
C_PAD = [surfer.rgb(38, 42, 52), surfer.rgb(230, 150, 60), surfer.rgb(250, 90, 70)]
C_PAD_BEAT = surfer.rgb(46, 51, 63)          # first pad of each beat, off state
C_PAD_PLAY = surfer.rgb(96, 106, 128)        # playhead column, off state
C_PANEL = surfer.rgb(26, 29, 36)
C_MARK = surfer.rgb(60, 90, 140)

state = {
    "bpm": 120, "shuffle": 0.0, "master": 0.8, "playing": False,
    "pattern": 0, "loop": True, "lane": 0, "step": 0,
    "patterns": [[[0] * NUM_STEPS for _ in range(NUM_CH)]
                 for _ in range(NUM_PATTERNS)],
    "fx": {},
}


class Pad:
    """One step cell: a rect + on_touch. Tap cycles 0/1/2; a drag is left
    for the scrollview to steal. The playhead brightens its column."""

    def __init__(self, parent, x, y, ch, step):
        self.ch, self.step = ch, step
        self.lit = False
        self.node = surfer.rect(x, y, 34, 44, C_PAD[0])
        parent.add(self.node)
        self.down_at = (0, 0)
        self.node.on_touch = self.touch
        self.refresh()

    def val(self):
        return state["patterns"][state["pattern"]][self.ch][self.step]

    def touch(self, phase, x, y):
        if phase == surfer.TOUCH_DOWN:
            self.down_at = (x, y)
        elif phase == surfer.TOUCH_UP:
            dx, dy = x - self.down_at[0], y - self.down_at[1]
            if dx * dx + dy * dy < 36:  # a tap, not a stolen scroll
                pat = state["patterns"][state["pattern"]]
                pat[self.ch][self.step] = (pat[self.ch][self.step] + 1) % 3
                self.refresh()

    def refresh(self):
        v = self.val()
        if v:
            c = C_PAD[v]
        elif self.lit:
            c = C_PAD_PLAY
        elif self.step % 4 == 0:
            c = C_PAD_BEAT
        else:
            c = C_PAD[0]
        self.node.set_color(c)


def fmt_bpm(v):
    return str(60 + int(v * 180))


def make_labeled_knob(parent, x, y, label, value, fmt=None, on=None):
    k = surfer.knob(x, y, 40)
    parent.add(k)
    k.value = value
    lbl = surfer.label(label, x, y + 42, C_DIM)
    lbl2 = None
    parent.add(lbl)
    if fmt:
        lbl2 = surfer.label(fmt(value), x, y + 58, C_TEXT)
        parent.add(lbl2)

        def cb(v, l=lbl2, f=fmt):
            l.set_text(f(v))
            if on:
                on(v)
        k.callback = cb
    elif on:
        k.callback = on
    return k


def main():
    surfer.init(W, H)
    root = surfer.screen()

    # ---------------- top bar (shared by both pages)
    root.add(surfer.rect(0, 0, W, 48, surfer.rgb(30, 33, 41)))
    root.add(surfer.label("Gamma", 16, 8, C_TITLE, surfer.FONT_UI28))
    root.add(surfer.label("9001", 106, 8, surfer.rgb(230, 150, 60),
                          surfer.FONT_UI28))
    root.add(surfer.label("surfer edition", 180, 18, C_DIM))

    pos_lbl = surfer.label("1.1", 420, 14, C_TEXT, surfer.FONT_UI28)
    root.add(pos_lbl)

    play = surfer.button(300, 9, 48, 30, "play")
    stop = surfer.button(352, 9, 48, 30, "stop")
    root.add(play)
    root.add(stop)

    bpm_lbl = surfer.label("bpm " + str(state["bpm"]), 510, 18, C_TEXT)
    root.add(bpm_lbl)
    bpm = surfer.knob(590, 4, 40)
    bpm.value = (state["bpm"] - 60) / 180
    root.add(bpm)

    def bpm_cb(v):
        state["bpm"] = 60 + int(v * 180)
        bpm_lbl.set_text("bpm " + str(state["bpm"]))
    bpm.callback = bpm_cb

    page_btn = surfer.button(880, 9, 128, 30, "controls >")
    root.add(page_btn)

    # ---------------- page 1: channel rows in a scrollview
    sv = surfer.scrollview(0, 52, W, H - 52)
    root.add(sv)

    pads = []          # [ch][step]
    ROW_H = 74
    for ch in range(NUM_CH):
        y = ch * ROW_H
        sv.add(surfer.rect(0, y, W - 16, ROW_H - 4, C_ROW[ch % 2]))
        name = surfer.label(KIT[ch], 10, y + 26, C_TEXT)
        sv.add(name)
        for i, kl in enumerate(CH_KNOBS):
            k = surfer.knob(112 + i * 46, y + 4, 40)
            k.value = CH_DEFAULTS[i]
            sv.add(k)
            sv.add(surfer.label(kl, 118 + i * 46, y + 46, C_DIM))
        row_pads = []
        for st in range(NUM_STEPS):
            row_pads.append(Pad(sv, 396 + st * 38, y + 12, ch, st))
        pads.append(row_pads)

    # a starter beat so the screen isn't blank (four on the floor-ish)
    pat0 = state["patterns"][0]
    for st in range(0, 16, 4):
        pat0[0][st] = 2 if st == 0 else 1
    for st in (4, 12):
        pat0[1][st] = 1
    for st in range(0, 16, 2):
        pat0[4][st] = 1
    for row in pads:
        for p in row:
            p.refresh()

    # ---------------- page 2: control panels
    page2 = surfer.group(0, 52)

    def panel(x, w, title):
        page2.add(surfer.rect(x, 0, w, H - 60, C_PANEL))
        page2.add(surfer.label(title, x + 10, 8, C_TITLE))
        return x

    # step seq
    px = panel(8, 236, "step seq")
    lane_mark = surfer.rect(px + 10, 40, 66, 28, C_MARK)
    page2.add(lane_mark)
    for i, name in enumerate(["Hit", "Vel", "Pitch"]):
        b = surfer.button(px + 12 + i * 70, 42, 62, 24, name)
        page2.add(b)

        def lane_cb(_v, idx=i):
            state["lane"] = idx
            lane_mark.x_pos = px + 10 + idx * 70
        b.callback = lane_cb
    make_labeled_knob(page2, px + 16, 96, "shuffle", 0.0,
                      lambda v: "%d%%" % int(v * 60))
    loop = surfer.checkbox(px + 16, 190)
    loop.value = True
    page2.add(loop)
    page2.add(surfer.label("loop", px + 52, 196, C_TEXT))

    def loop_cb(v):
        state["loop"] = v
    loop.callback = loop_cb

    # pattern
    px = panel(252, 236, "pattern")
    pat_mark = surfer.rect(px + 8, 38, 54, 32, C_MARK)
    page2.add(pat_mark)

    def select_pattern(idx):
        state["pattern"] = idx
        pat_mark.x_pos = px + 8 + (idx % 4) * 56
        pat_mark.y_pos = 38 + (idx // 4) * 36
        for row in pads:
            for p in row:
                p.refresh()

    for i in range(NUM_PATTERNS):
        b = surfer.button(px + 10 + (i % 4) * 56, 40 + (i // 4) * 36, 50, 28,
                          str(i + 1))
        page2.add(b)

        def pat_cb(_v, idx=i):
            select_pattern(idx)
        b.callback = pat_cb

    copy_b = surfer.button(px + 10, 196, 62, 26, "copy")
    clear_b = surfer.button(px + 78, 196, 62, 26, "clear")
    page2.add(copy_b)
    page2.add(clear_b)
    clipboard = [[0] * NUM_STEPS for _ in range(NUM_CH)]

    def copy_cb(_v):
        pat = state["patterns"][state["pattern"]]
        for c in range(NUM_CH):
            clipboard[c] = list(pat[c])

    def clear_cb(_v):
        pat = state["patterns"][state["pattern"]]
        for c in range(NUM_CH):
            for s in range(NUM_STEPS):
                pat[c][s] = 0
        for row in pads:
            for p in row:
                p.refresh()
    copy_b.callback = copy_cb
    clear_b.callback = clear_cb

    mode = surfer.dropdown(px + 10, 236, 140, ["pattern", "song"])
    page2.add(mode)

    # effects
    px = panel(492, 260, "effects")
    fx_defs = [("Reverb", 0.0), ("Live", 0.85), ("Chorus", 0.0),
               ("Echo", 0.0), ("Delay", 0.25), ("Fdbk", 0.3),
               ("EQ Lo", 0.5), ("EQ Mid", 0.5), ("EQ Hi", 0.5)]
    for i, (name, dv) in enumerate(fx_defs):
        kx = px + 20 + (i % 3) * 80
        ky = 44 + (i // 3) * 96
        state["fx"][name] = dv

        def fx_cb(v, n=name):
            state["fx"][n] = v
        make_labeled_knob(page2, kx, ky, name, dv,
                          lambda v: "%.2f" % v, fx_cb)

    # global
    px = panel(760, 248, "global")
    def master_cb(v):
        state["master"] = v * 1.2
    make_labeled_knob(page2, px + 20, 44, "master", state["master"] / 1.2,
                      lambda v: "%.2f" % (v * 1.2), master_cb)
    share = surfer.button(px + 20, 160, 90, 28, "share")
    page2.add(share)

    def share_cb(_v):
        print("share: pattern %d, bpm %d" % (state["pattern"] + 1, state["bpm"]))
    share.callback = share_cb
    page2.add(surfer.label("UI test build - no audio.\nAMY hookup comes next.",
                           px + 16, 210, C_DIM))

    # ---------------- page switching (detach/reattach, tulip-style)
    current = ["pattern"]

    def show_page(which):
        current[0] = which
        if which == "pattern":
            page2.detach()
            root.add(sv)
            page_btn.label = "controls >"
        else:
            sv.detach()
            root.add(page2)
            page_btn.label = "< pattern"

    def page_cb(_v):
        show_page("controls" if current[0] == "pattern" else "pattern")
    page_btn.callback = page_cb

    # ---------------- transport
    def play_cb(_v):
        state["playing"] = True
        state["step"] = 0
        state["_next"] = time.ticks_ms()

    def stop_cb(_v):
        state["playing"] = False
        set_playhead(-1)
        pos_lbl.set_text("1.1")
    play.callback = play_cb
    stop.callback = stop_cb

    lit_col = [-1]

    def set_playhead(col):
        old = lit_col[0]
        lit_col[0] = col
        for row in pads:
            if old >= 0:
                row[old].lit = False
                row[old].refresh()
            if col >= 0:
                row[col].lit = True
                row[col].refresh()

    # ---------------- main loop
    try:
        import os
        shot = os.getenv("SURF_SHOT")
    except (ImportError, AttributeError):
        shot = None
    frames = 0

    while surfer.tick():
        for k in surfer.keys():
            pass
        if shot:
            frames += 1
            if frames == 2:
                play_cb(1)
            elif frames == 10:  # tap the Clap row, step 2: toggles via dispatch
                surfer._touch(451, 236, surfer.TOUCH_DOWN)
                surfer._touch(451, 236, surfer.TOUCH_UP)
            elif frames == 12:
                v = state["patterns"][0][2][1]
                print("pad tap -> state:", "OK" if v == 1 else "FAIL (%r)" % v)
            elif frames == 40:
                surfer.screenshot(shot + "_p1.ppm")
                show_page("controls")
            elif frames == 60:
                surfer.screenshot(shot + "_p2.ppm")
                return
        if state["playing"]:
            now = time.ticks_ms()
            if time.ticks_diff(now, state["_next"]) >= 0:
                step = state["step"]
                set_playhead(step)
                pos_lbl.set_text("%d.%d" % (step // 4 + 1, step % 4 + 1))
                interval = 60000 // state["bpm"] // 4
                if step % 2 == 1:  # shuffle delays the off-16ths
                    interval += int(interval * state.get("shuffle", 0))
                state["_next"] = time.ticks_add(state["_next"], interval)
                state["step"] = (step + 1) % NUM_STEPS


main()
