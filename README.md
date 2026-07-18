# surfer

**High-performance UI library for the ESP32-P4 and framebuffers.**

surfer is a small retained-mode UI compositor: C11 core, backends for the
ESP32-P4 (MIPI DSI + PPA), desktop (SDL2), and — later — web (emscripten).
MicroPython bindings are on the roadmap. It's being built as the next
generation UI for [Tulip](https://tulip.computer).

> ⚠️ **Early days.** This is an architecture experiment with working code and
> real measurements, not a finished library. The API will change. We're
> currently through M3 of the milestone plan in [DESIGN.md](DESIGN.md) —
> which is the source of truth for how everything works and why.

![the mixer demo: 6 knobs + 6 sliders with labels](docs/mixer.png)

*The mixer demo — 6 filmstrip knobs + 6 sliders, draggable by mouse or
finger, with baked-atlas labels. This exact scene runs at 63–66 fps under
finger on an ESP32-P4, every pixel composited by the PPA. (Screenshots are
straight framebuffer dumps, `SURF_SHOT=x.ppm`.)*

![the text demo: wrap, ellipsis, textinput with caret](docs/text.png)

*M3 text: greedy word wrap, ellipsize, and an editable textinput with
caret, selection, and scroll-into-view. Glyphs are stb_truetype-baked A8
atlases; drawing text is the same clipped-blit path as everything else.*

![the settings demo: scrollview, checkboxes, dropdown](docs/settings.png)

*M4: a scrollable settings panel (flick momentum + edge spring-back run
in core ticks) with checkboxes, sliders, and a dropdown whose popup
overlays via detach/reattach. Scrolling steals taps after an 8px
threshold; slider and knob drags are never stolen.*

## The idea

At 1080p RGB565, one frame is ~4 MB. On an ESP32-P4 the framebuffer lives in
PSRAM behind a few hundred MB/s of shared bandwidth — we measured CPU writes
at ~87 MB/s and the PPA's fastest engines at ~360 MB/s. Full-frame redraw at
60 fps is arithmetic you cannot win. So surfer never tries:

- **Never redraw the full frame.** Dirty-rect composition only. Moving a
  knob damages one small rect; that's the whole frame's work.
- **Never rasterize at frame time.** Widget visuals are pre-rendered assets:
  filmstrips (a knob is 64 baked frames), 9-slice patches, baked font
  atlases (coming in M3). If a widget "needs" runtime drawing, the answer is
  a better asset.
- **The frame path is blits.** Fill / blit / alpha-blend, executed by the
  P4's PPA + 2D-DMA on device, by a small software loop on desktop. The hal
  is a ~10-function vtable; everything above it is platform-free C11.

## vs. LVGL

LVGL is a great, mature, general-purpose library — surfer is deliberately
not that. The trade-offs we're betting on:

| | LVGL | surfer |
|---|---|---|
| Frame path | software rasterizer, PPA accelerates ~30% of it (experimental) | 100% hal blits — the PPA executes the entire frame path |
| Widget visuals | drawn at runtime (vector-ish styles) | pre-rendered assets, composited |
| Text | runtime FreeType/tiny_ttf or pre-baked | atlases baked at build time, drawing is blits (M3) |
| Cache coherency on P4 | scattered, a known source of bugs | owned in one place in the hal (`surf_hal_p4_sync`) |
| API surface | thousands of symbols, generated bindings | ~8 node types, ~60 functions, hand-written bindings |
| Multitasking UIs | manual | subtrees detach/reattach losslessly — an app's UI is one group |
| Maturity | production, huge ecosystem | **experiment in progress** |

## Measured (ESP32-P4-Function-EV-Board, 1024×600 DSI, IDF 5.4)

The M2 milestone was "prove the whole bet on hardware, or stop." Numbers
from the built-in benchmark and the 6-knobs + 6-sliders demo:

- Finger drag on the mixer demo: **63–66 fps sustained, ~2 ms/tick** (~7×
  headroom against the frame budget)
- Pathological worst case (every widget + full-screen repaint every frame):
  ~18 ms/tick, ~55 fps
- Presentation: triple-buffer-with-damage — zero-copy DSI flips + DMA2D
  damage-forward. Flicker- and tear-free. The measurements that picked it
  over single- and double-buffering are in DESIGN.md §5.2.
- PPA per-op overhead is ~70–200 µs regardless of size, which reshaped the
  asset rules: bake art at final size when you can (one blit beats 110
  tiled ones by ~12 ms).

## Layout & building

```
include/surfer.h        public API (~20 functions so far, the binding surface)
src/core/               scene graph, dirty rects, hit test, input capture
src/widgets/            knob, slider (button, checkbox, dropdown... later)
src/hal/sdl/            desktop backend (per-pixel code lives only here)
src/hal/p4/             ESP32-P4 backend: PPA, DSI flips, DMA2D, GT911 touch
ports/esp32p4/          ESP-IDF project (Function-EV-Board)
tools/                  build-time asset bakers
demos/                  mixer (M1/M2 demo), bounce (M0 demo)
```

Desktop (needs SDL2 + python3):

```
make sdl && ./build/surfer_demo    # 6 knobs + 6 sliders, drag with mouse
make test                          # unit tests, no SDL needed
```

Device (needs ESP-IDF ≥ 5.4, ESP32-P4-Function-EV-Board with the 7" panel):

```
cd ports/esp32p4
idf.py -p <port> flash monitor     # boots into a bench, then the demo
```

## Status

- [x] M0 — hal vtable, SDL backend, dirty-rect compositor
- [x] M1 — filmstrip/9-patch nodes, knob + slider, touch capture
- [x] M2 — P4 backend: PPA, DSI, buffering benchmark, 60 fps under finger
- [x] M3 — text: baked font atlases, label, wrap, textinput + caret
- [x] M4 — scrollview + momentum, checkbox, dropdown
- [ ] M5 — MicroPython bindings
- [ ] M6 — web build (emscripten), real art pass, default theme

## License

MIT
