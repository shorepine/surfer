# surfer — design

A small retained-mode UI compositor for Tulip's next generation. Replaces LVGL.
Targets: ESP32-P4 (rev 3) over MIPI DSI, desktop via SDL2, web via emscripten+SDL2.
Core in C11. Scripted from MicroPython (device) and CPython (desktop, later).

Codename rationale: the whole job is riding the memory bandwidth wave without
wiping out.

## 1. The constraint that shapes everything

1080p RGB565 is ~3.96 MB per frame. Full-frame software redraw at 60 fps means
~250 MB/s of writes plus comparable reads against PSRAM with roughly ~400 MB/s
theoretical bandwidth (32 MB embedded octal PSRAM @ 200 MHz), shared with
everything else on the chip. Conclusion, baked into the architecture:

- **Never redraw the full frame per tick.** Dirty-rect composition only.
- **Never rasterize vectors at frame time.** All widget visuals are
  pre-rendered assets (filmstrips, 9-slice patches, baked font atlases).
- **The frame path is blits.** Fill / blit / alpha-blend / scale, executed by
  the P4's PPA + 2D-DMA on device, by memcpy/SDL on desktop and web.

The P4's PPA is a blitter (fill, blend, scale, rotate), not a rasterizer.
Designing for "everything is a blit" means the hardware accelerates 100% of the
frame path instead of the ~30% LVGL manages.

A knob drag therefore costs: 1 background patch blit + 1 filmstrip frame blit
into one small dirty rect. That is how 60 fps finger tracking is guaranteed by
construction.

## 2. Layers

```
┌─────────────────────────────────────────────┐
│ bindings: micropython (device) / cpython    │
├─────────────────────────────────────────────┤
│ widgets: knob slider button checkbox        │
│          dropdown label textinput scrollview│
├─────────────────────────────────────────────┤
│ core: scene graph, dirty rects, hit test,   │
│       animation ticks, asset cache, text    │
├─────────────────────────────────────────────┤
│ hal: fill/blit/blend/scale, present, touch, │
│      time  →  p4 / sdl / web backends       │
└─────────────────────────────────────────────┘
```

Everything above the hal is platform-independent C. The hal is a small vtable
(~10 functions). Rule: platform #ifdefs live only in `src/hal/`.

### 2.1 hal interface (sketch)

```c
typedef struct {
    void (*fill)(surf_rect dst, surf_color c);
    void (*blit)(const surf_image *src, surf_rect src_r, surf_point dst);
    void (*blend)(const surf_image *src, surf_rect src_r, surf_point dst, uint8_t opa);
    void (*scale_blit)(const surf_image *src, surf_rect src_r, surf_rect dst_r);
    void (*present)(const surf_rect *dirty, int n);   // flush rects to panel
    void (*wait_idle)(void);                          // fence on PPA/DMA queue
    uint64_t (*now_us)(void);
    bool (*poll_touch)(surf_touch *out);
    void *(*alloc_image)(size_t bytes);               // 64B-aligned, PSRAM ok
    void (*free_image)(void *p);
} surf_hal;
```

Backends:
- **p4**: PPA SRM/blend/fill units + 2D-DMA, `esp_lcd` MIPI DSI panel path.
  All buffers 64-byte (cache line) aligned; explicit `esp_cache_msync` on
  CPU-written regions before PPA reads them. This is the exact class of bug
  that plagues LVGL's experimental PPA path — we own it centrally in the hal,
  once.
- **sdl**: SDL2 texture streaming; software blend via a small, vectorizable
  C loop (desktop CPUs don't care).
- **web**: the sdl backend compiled with emscripten. Zero new code initially.

### 2.2 Core: scene graph

Retained tree of nodes. Small fixed set of node types (widgets are built from
these; users mostly never see them):

| node       | payload                              |
|------------|--------------------------------------|
| group      | children, x/y offset, clip flag      |
| rect       | solid fill (flat backgrounds)        |
| sprite     | image ref, src rect                  |
| filmstrip  | image ref, frame w/h, frame index    |
| ninepatch  | image ref, insets, dst w/h           |
| text       | string, font ref, wrap width, align  |
| textinput  | text + caret/selection state         |
| scrollview | group + content offset + momentum    |

Node property writes mark the node's old and new screen rects dirty. That's the
entire invalidation model.

**Tree semantics (the LVGL feature worth keeping):** subtrees detach and
reattach losslessly. `node.detach()` removes a subtree from the render tree but
keeps it fully alive — state, children, everything. `parent.add(node)` puts it
back. This is the multitasking primitive: an app's whole UI is one group that
Tulip's switcher detaches and re-attaches. No serialize/deserialize needed for
the common case; a save-to-disk form can come later if ever needed.

### 2.3 Render loop

Per tick (driven by vsync/tear-effect signal on device, SDL timer elsewhere):

1. Run animation/momentum ticks (may dirty more rects).
2. Coalesce dirty rects (merge overlapping; cap list at N=16, degrade to
   bounding union).
3. For each rect: walk the tree front-to-back to find the topmost opaque
   coverage, then composite back-to-front from there using hal ops, clipped to
   the rect.
4. `present(rects)`.

Buffering on device: two full framebuffers in PSRAM, DSI scans out one while we
composite into the other, damage copied forward (PPA blit) before compositing —
standard double-buffer-with-damage. If bandwidth measurements say full double
buffering is too hungry at 1080p, fall back to single buffer + present
synchronized against the tear-effect line. Decide with a benchmark, not vibes
(see M2).

### 2.4 Assets

- Build-time packer (`tools/surfpack.py`): PNGs in, one `.surf` pack out —
  header + raw pixel data in the device's native format, each image 64B-aligned
  within the pack so it can be DMA'd straight from mapped flash or loaded to
  PSRAM without fixup.
- Knobs are filmstrips (e.g. 128 frames, rendered from Blender — actual raked
  metal, real shadows). Sliders are 9-slice tracks + a cap sprite. States
  (pressed/disabled/checked) are just other frames/images.
- Default theme ships in the firmware image; user themes load from Tulip's
  filesystem at runtime.

### 2.5 Text (the hard part — decisions pinned here)

- **Rasterization: build time, not frame time.** `stb_truetype` bakes glyph
  atlases per (font, size) into the asset pack. Runtime text drawing is
  A8-atlas blits — same frame path as everything else. Baking per exact pixel
  size is also the fix for "fonts look bad at low res": no runtime-scaled
  antialiasing soup.
- Wrapping: greedy word wrap with per-glyph advance tables, break on
  space/hyphen, ellipsize option. Kerning pairs from the bake. No bidi, no
  complex shaping in v1 (Latin/Greek/Cyrillic coverage; CJK later via bigger
  atlases — the format shouldn't preclude it, so glyph IDs are 32-bit
  codepoints from day one).
- textinput: caret, selection, insert/delete, horizontal scroll-into-view,
  on-screen keyboard is a *widget built from surfer parts*, not core code.
- Escape hatch: an optional on-device baker (stb_truetype compiled in) so users
  can load a TTF at a size we didn't ship. Off the frame path — it bakes an
  atlas once, then rendering is blits again.

### 2.6 Input

Touch → hit test (front-to-back walk, honoring clip) → widget gets
down/move/up. A widget may **capture** the pointer on down (sliders/knobs do),
so drags keep tracking after the finger leaves the widget's rect — the standard
fix for the "slider loses the finger" feel. Knob drag mapping: vertical-drag
relative mode by default (like every DAW), configurable to angular.
scrollview: threshold before scroll steals the gesture from children;
momentum + edge resistance handled in core so every backend feels identical.

## 3. Bindings

MicroPython first. The C API is designed to be bound: one public header
(`surfer.h`), flat functions, integer handles or single-struct pointers, no
callbacks-of-callbacks. Given the API is deliberately tiny (~8 node types,
~60 functions), **hand-write the MicroPython module** (`modsurfer.c`) rather
than building a generator — a generator is what you need when wrapping LVGL's
thousands of symbols, not this. Revisit only if the API sprawls (it shouldn't;
that's the point).

Python-side feel (the acceptance test for the API):

```python
import surfer

ui = surfer.Group()
for i, name in enumerate(["cutoff", "res", "env", "lfo", "mix", "vol"]):
    s = surfer.Slider(x=20 + i * 110, y=40, w=90, h=280, label=name)
    s.on_change = lambda v, n=name: synth.set(n, v)
    ui.add(s)
surfer.screen().add(ui)

# multitasking:
ui.detach()          # app hidden, state intact
surfer.screen().add(ui)  # and it's back
```

Callbacks fire from the surfer tick into MicroPython on the same task —
no cross-task marshaling in v1; Tulip already runs MP on a known core/task and
the render tick will live there too. (If profiling later says rendering wants
its own task, only `present`/PPA waits move, not callbacks.)

## 4. Milestones

- **M0** — repo scaffold, hal vtable, sdl backend, sprite+rect+group nodes,
  dirty-rect compositor, demo: bouncing sprites. *Proves the loop.*
- **M1** — filmstrip + ninepatch, knob + slider widgets with placeholder art,
  touch capture, drag mapping. Demo: 6 sliders + 6 knobs at 60 fps under mouse
  drag on desktop. *Proves the feel.*
- **M2** — p4 hal backend: PPA fill/blend/blit, 2D-DMA, DSI present, buffering
  strategy benchmark (double-buffer+damage vs single+TE) at target resolution.
  Same demo on hardware at 60 fps under finger. *Proves the whole bet — do not
  build past this until it passes.*
- **M3** — text: surfpack font baking, label, wrap, textinput + caret.
- **M4** — scrollview + momentum, checkbox, dropdown.
- **M5** — micropython bindings, run the M1 demo from Python on device.
- **M6** — web build (emscripten), real knob/slider art pass, default theme.

## 5. Open questions (flagged for veto)

1. **Framebuffer format: RGB565 vs RGB888.** Proposal: RGB565 framebuffer
   (halves bandwidth — this likely *is* the 60fps margin at 1080p), assets
   stored ARGB8888 and blended down by the PPA, plus an ordered-dither pass in
   the surfpack baker for large soft gradients (the metal-knob banding risk
   lives in the assets, so kill it at bake time). Escape hatch: format is a
   hal-level property, so an RGB888 build stays possible for smaller panels.
2. **Buffering on device** — DECIDED at M2 (measured on P4 + EK79007
   1024×600): **single buffer, composite directly into the DSI scanout fb.**
   Damage-copy present costs ~20 ms/full-screen because the PPA SRM engine
   copies at only ~123 MB/s (r+w); fill and blend engines hit ~360 MB/s.
   Direct composition ran the full-scene mixer at 10 ms/frame and a
   single-finger drag at ~2.3 ms/frame (62–66 fps sustained under finger).
   No visible tearing with dirty-rect-sized updates. Revisit only if a
   full-screen animation shows tearing.

   M2 also measured **PPA per-op overhead ≈ 70–200 µs regardless of size**
   (64×64 blend = 169 µs, of which ~half is setup). Consequence, now a
   design rule: assets that can be baked at final size are baked at final
   size (one blit); tiled 9-patch stretching is for cold paths only — a
   36×36 track tiled to 48×330 was ~110 ops ≈ 12 ms, the baked equivalent
   is 1 op. Full-frame 1080p ops measured 11–67 ms — full-frame redraw is
   as impossible as §1 predicted; CPU PSRAM writes ~87 MB/s.
3. **Filmstrip frame count vs memory.** 128 frames × 96×96 × ARGB8888 ≈ 4.7 MB
   per knob style — too fat. Options: 64 frames + nearest-frame (audio
   hardware convention, imperceptible), 96×96 RGB565+A8 (~2.3 MB), or
   per-style budget in surfpack that errors when a theme blows the cap.
   Proposal: 64 frames, RGB565+A8, cap enforced by the packer.
4. **Does `scale_blit` make v1?** PPA does scaling/rotation nearly free; SDL
   too. Tempting for zoom transitions, but cut from v1 frame path to keep the
   compositor simple — the hal op exists, nothing uses it yet.
5. **License.** Everything here can be MIT (stb is public domain/MIT). Decide
   before first public push.
