# Gamma 9001, surfer edition — the AMY drum machine's UI rebuilt in
# MicroPython on surfer (UI only; no sound yet). The layout follows the
# web app: 8 channels x 16 steps with per-channel ganged knobs (dial +
# label + live value readout in real units, like the web's makeKnob),
# and the step-seq / pattern / effects / global panels on a second page
# behind the "controls" button. Channel rows scroll vertically.
#
# Ganged-knob interactions, mirroring the web:
#   - drag the knob: value readout tracks in real units (+7, 1.2k, OPEN)
#   - tap the value text: reset to default (the web's dblclick)
#   - gk.set(real_value) from code: dial and readout both follow
#
# Desktop:  ~/micropython/ports/unix/build-standard/micropython \
#               bindings/surfer/examples/gamma9001.py
# Device:   frozen in; `import gamma9001`.
import math
import time
import surfer

W, H = 1024, 600
NUM_CH = 8
NUM_STEPS = 16
NUM_PATTERNS = 16
ROW_H = 92

KIT = ["909 Kick", "909 Snare", "909 Clap", "Conga",
       "909 CH", "909 OH", "Cowbell", "909 Ride"]

C_BG = surfer.rgb(18, 20, 25)
C_ROW = [surfer.rgb(26, 29, 36), surfer.rgb(30, 33, 41)]
C_TEXT = surfer.rgb(200, 205, 215)
C_DIM = surfer.rgb(140, 146, 158)
C_TITLE = surfer.rgb(240, 242, 248)
C_VAL = surfer.rgb(230, 180, 110)
C_PAD = [surfer.rgb(38, 42, 52), surfer.rgb(230, 150, 60), surfer.rgb(250, 90, 70)]
C_PAD_BEAT = surfer.rgb(46, 51, 63)
C_PAD_PLAY = surfer.rgb(96, 106, 128)
C_PANEL = surfer.rgb(26, 29, 36)
C_MARK = surfer.rgb(60, 90, 140)

LOG_CUT_MIN = math.log(50, 2)
LOG_CUT_MAX = math.log(16000, 2)


def fmt_hz(v):
    hz = int(2 ** v)
    if hz >= 15800:
        return "OPEN"
    if hz >= 1000:
        return "%.1fk" % (hz / 1000)
    return str(hz)


# per-channel knob defs: (label, lo, hi, default, fmt) — from the web app
CH_KNOB_DEFS = [
    ("Pit", -24, 24, 0, lambda v: "%+d" % int(round(v))),
    ("Off", 0.0, 0.95, 0.0, lambda v: "%.2f" % v),
    ("Pan", 0.0, 1.0, 0.5, lambda v: "%.2f" % v),
    ("Vol", 0.0, 1.2, 0.8, lambda v: "%.2f" % v),
    ("Cut", LOG_CUT_MIN, LOG_CUT_MAX, LOG_CUT_MAX, fmt_hz),
    ("Res", 0.1, 8.0, 0.7, lambda v: "%.2f" % v),
]

state = {
    "bpm": 120, "shuffle": 0.0, "master": 0.8, "playing": False,
    "pattern": 0, "loop": True, "lane": 0, "step": 0,
    "patterns": [[[0] * NUM_STEPS for _ in range(NUM_CH)]
                 for _ in range(NUM_PATTERNS)],
    "channels": [{d[0]: d[3] for d in CH_KNOB_DEFS} for _ in range(NUM_CH)],
    "fx": {},
}


def tappable(node, fn):
    """Call fn() on a tap (release near the press) — drags stay scrolls."""
    down = [0, 0]

    def touch(phase, x, y):
        if phase == surfer.TOUCH_DOWN:
            down[0], down[1] = x, y
        elif phase == surfer.TOUCH_UP:
            dx, dy = x - down[0], y - down[1]
            if dx * dx + dy * dy < 36:
                fn()
    node.on_touch = touch


class GKnob:
    """The web app's ganged knob: 40px dial + name + live value readout,
    working in real units. Drag sets, tap the value resets to default,
    .set() from code moves both dial and text."""

    def __init__(self, parent, x, y, label, lo, hi, default, fmt, on=None):
        self.lo, self.hi, self.default = lo, hi, default
        self.fmt = fmt
        self.on = on
        self.k = surfer.knob(x, y, 40)
        parent.add(self.k)
        # labels center in a 56px box around the 40px knob
        self.name = surfer.label(label, x - 8, y + 42, C_DIM)
        self.name.set_wrap(56)
        self.name.set_align(surfer.ALIGN_CENTER)
        parent.add(self.name)
        self.val = surfer.label("", x - 8, y + 58, C_VAL)
        self.val.set_wrap(56)
        self.val.set_align(surfer.ALIGN_CENTER)
        parent.add(self.val)
        tappable(self.val, self.reset)
        self.k.callback = self._turned
        self.set(default, fire=False)

    def real(self):
        return self.lo + self.k.value * (self.hi - self.lo)

    def _turned(self, frac):
        v = self.lo + frac * (self.hi - self.lo)
        self.val.set_text(self.fmt(v))
        if self.on:
            self.on(v)

    def set(self, v, fire=True):
        v = min(self.hi, max(self.lo, v))
        self.k.value = (v - self.lo) / (self.hi - self.lo)
        self.val.set_text(self.fmt(v))
        if fire and self.on:
            self.on(v)

    def reset(self):
        self.set(self.default)


class Pad:
    """One step cell: rect + on_touch; tap cycles off/hit/accent."""

    def __init__(self, parent, x, y, ch, step):
        self.ch, self.step = ch, step
        self.lit = False
        self.node = surfer.rect(x, y, 34, 44, C_PAD[0])
        parent.add(self.node)
        tappable(self.node, self.cycle)
        self.refresh()

    def val(self):
        return state["patterns"][state["pattern"]][self.ch][self.step]

    def cycle(self):
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


def main():
    surfer.init(W, H)
    root = surfer.screen()

    # ---------------- top bar (shared by both pages)
    root.add(surfer.rect(0, 0, W, 48, surfer.rgb(30, 33, 41)))
    root.add(surfer.label("Gamma", 16, 8, C_TITLE, surfer.FONT_UI28))
    root.add(surfer.label("9001", 106, 8, surfer.rgb(230, 150, 60),
                          surfer.FONT_UI28))

    pos_lbl = surfer.label("1.1", 420, 14, C_TEXT, surfer.FONT_UI28)
    root.add(pos_lbl)

    play = surfer.button(230, 9, 48, 30, "play")
    stop = surfer.button(282, 9, 48, 30, "stop")
    root.add(play)
    root.add(stop)

    bpm = GKnob(root, 530, 3, "", 60, 240, 120,
                lambda v: "bpm %d" % int(v),
                lambda v: state.__setitem__("bpm", int(v)))
    # the top bar is only 48px: park the readout beside the dial instead
    bpm.val.x_pos = 578
    bpm.val.y_pos = 16
    bpm.val.set_wrap(90)
    bpm.val.set_align(surfer.ALIGN_LEFT)

    page_btn = surfer.button(880, 9, 128, 30, "controls >")
    root.add(page_btn)

    # ---------------- page 1: channel rows in a scrollview
    sv = surfer.scrollview(0, 52, W, H - 52)
    root.add(sv)
    sv.fast_scroll(True)  # fullscreen-width viewport, unoccluded

    pads = []
    ch_knobs = []  # [ch][i] -> GKnob, for programmatic gang demo
    for ch in range(NUM_CH):
        y = ch * ROW_H
        sv.add(surfer.rect(0, y, W - 16, ROW_H - 4, C_ROW[ch % 2]))
        sv.add(surfer.label(KIT[ch], 10, y + 36, C_TEXT))
        row_knobs = []
        for i, (lbl, lo, hi, dv, fmt) in enumerate(CH_KNOB_DEFS):
            kx = 112 + i * 46

            def on_change(v, c=ch, key=CH_KNOB_DEFS[i][0]):
                state["channels"][c][key] = v
            gk = GKnob(sv, kx, y + 2, lbl, lo, hi, dv, fmt, on_change)
            row_knobs.append(gk)
        ch_knobs.append(row_knobs)
        row_pads = []
        for st in range(NUM_STEPS):
            row_pads.append(Pad(sv, 396 + st * 38, y + 22, ch, st))
        pads.append(row_pads)

    # starter beat
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
    GKnob(page2, px + 24, 96, "shuffle", 0.0, 0.6, 0.0,
          lambda v: "%d%%" % int(v * 100),
          lambda v: state.__setitem__("shuffle", v))
    loop = surfer.checkbox(px + 16, 200)
    loop.value = True
    page2.add(loop)
    page2.add(surfer.label("loop", px + 52, 206, C_TEXT))
    loop.callback = lambda v: state.__setitem__("loop", v)

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

    px = panel(492, 260, "effects")
    # (label, lo, hi, default, fmt) — real units like the web app
    fx_defs = [
        ("Reverb", 0.0, 1.0, 0.0, lambda v: "%.2f" % v),
        ("Live", 0.0, 1.0, 0.85, lambda v: "%.2f" % v),
        ("Chorus", 0.0, 1.0, 0.0, lambda v: "%.2f" % v),
        ("Echo", 0.0, 1.0, 0.0, lambda v: "%.2f" % v),
        ("Delay", 30.0, 1000.0, 250.0, lambda v: "%dms" % int(v)),
        ("Fdbk", 0.0, 0.9, 0.3, lambda v: "%.2f" % v),
        ("EQ Lo", -12.0, 12.0, 0.0, lambda v: "%+.1f" % v),
        ("EQ Mid", -12.0, 12.0, 0.0, lambda v: "%+.1f" % v),
        ("EQ Hi", -12.0, 12.0, 0.0, lambda v: "%+.1f" % v),
    ]
    for i, (name, lo, hi, dv, fmt) in enumerate(fx_defs):
        kx = px + 28 + (i % 3) * 82
        ky = 44 + (i // 3) * 96

        def fx_cb(v, n=name):
            state["fx"][n] = v
        state["fx"][name] = dv
        GKnob(page2, kx, ky, name, lo, hi, dv, fmt, fx_cb)

    px = panel(760, 248, "global")
    master = GKnob(page2, px + 28, 44, "master", 0.0, 1.2, 0.8,
                   lambda v: "%.2f" % v,
                   lambda v: state.__setitem__("master", v))
    share = surfer.button(px + 20, 160, 90, 28, "share")
    page2.add(share)
    share.callback = lambda _v: print(
        "share: pattern %d, bpm %d" % (state["pattern"] + 1, state["bpm"]))
    page2.add(surfer.label("UI test build - no audio.\n"
                           "tap a value to reset it.", px + 16, 210, C_DIM))

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

    page_btn.callback = lambda _v: show_page(
        "controls" if current[0] == "pattern" else "pattern")

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

    # v-scroll fps meter (serial only): first ~8s auto-scrolls the channel
    # list as a bench, then reports whenever the list is actively scrolling
    bench = [0, 4]
    last_off = 0
    t_prev = time.ticks_ms()
    ms_sum = n_frames = n_scrolled = 0

    while surfer.tick():
        now = time.ticks_ms()
        ms_sum += time.ticks_diff(now, t_prev)
        t_prev = now
        n_frames += 1
        off_y = sv.scroll_offset()[1]
        if off_y != last_off:
            n_scrolled += 1
        last_off = off_y
        if ms_sum >= 1000:
            if n_scrolled > n_frames // 4:
                print("vscroll: %.1f fps (%.1f ms/frame)" %
                      (n_frames * 1000.0 / ms_sum, ms_sum / n_frames))
            ms_sum = n_frames = n_scrolled = 0

        if not shot and frames < 400 and current[0] == "pattern":
            frames += 1
            y = bench[0] + bench[1]
            if y >= 188 or y <= 0:
                bench[1] = -bench[1]
            bench[0] = max(0, min(188, y))
            sv.scroll_to(0, bench[0])

        for k in surfer.keys():
            pass
        if state["playing"]:
            if time.ticks_diff(now, state["_next"]) >= 0:
                step = state["step"]
                set_playhead(step)
                pos_lbl.set_text("%d.%d" % (step // 4 + 1, step % 4 + 1))
                interval = 60000 // state["bpm"] // 4
                if step % 2 == 1:
                    interval += int(interval * state["shuffle"])
                state["_next"] = time.ticks_add(state["_next"], interval)
                state["step"] = (step + 1) % NUM_STEPS

        if shot:
            frames += 1
            if frames == 2:
                play_cb(1)
            elif frames == 6:
                # gang check: programmatic set moves dial + readout + state
                ch_knobs[0][0].set(7)       # kick pitch -> "+7"
                ch_knobs[0][4].set(LOG_CUT_MIN + 0.62 * (LOG_CUT_MAX - LOG_CUT_MIN))
            elif frames == 8:
                ok1 = abs(state["channels"][0]["Pit"] - 7) < 0.6
                ok2 = abs(ch_knobs[0][0].k.value - (7 + 24) / 48.0) < 0.02
                print("gang set -> state+dial:", "OK" if ok1 and ok2 else "FAIL")
            elif frames == 10:  # tap the value text: resets to default
                surfer._touch(120, 52 + 2 + 58 + 8, surfer.TOUCH_DOWN)
                surfer._touch(120, 52 + 2 + 58 + 8, surfer.TOUCH_UP)
            elif frames == 12:
                v = state["channels"][0]["Pit"]
                print("tap value -> reset:", "OK" if abs(v) < 0.6 else "FAIL (%r)" % v)
            elif frames == 40:
                surfer.screenshot(shot + "_p1.ppm")
                show_page("controls")
            elif frames == 60:
                surfer.screenshot(shot + "_p2.ppm")
                return


main()
