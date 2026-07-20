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

### 2.7 Shapes (load-time vector rasterization)

Vector shapes exist as a bake-time tool only (`src/core/shape.c`):
`surf_image_poly/polyline/ellipse/bezier` rasterize anti-aliased fills
and round-capped strokes INTO images, with solid or linear-gradient
paints (`surf_paint`). One scanline coverage engine (nonzero winding,
subpaths normalized to one orientation so stroke quads + join discs
union additively; 4x vertical supersampling, exact horizontal span
coverage). The frame path never sees a curve — a drawn image is sprite
and layer material like any PNG, so the "no runtime vector
rasterization" rule stands: this is how the better asset gets made.
Drawing into an A8 image puts coverage in alpha, which composes with
tint cycling: shapes whose color animates in hardware for the price of
the blend they already cost. Float math and malloc are allowed inside
shape.c for the same reason they're allowed in image.c: bake-time code.

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
   1024×600): **triple-buffer-with-damage.** Compose into a buffer that is
   neither on glass nor pending, zero-copy flip via
   `esp_lcd_panel_draw_bitmap` (an in-fb pointer just switches scanout at
   the next frame boundary — track which buffer went live in the
   refresh-done ISR), then DMA2D (`esp_async_fbcpy`) the last two frames'
   damage forward into the new back buffer, skipping rects covered by the
   current frame. Never stalls, and only fully-composed frames ever reach
   the glass. Numbers: finger drag 63–66 fps at ~2 ms/tick (3.1 ms max);
   pathological full-screen-every-frame animation 18 ms/tick (~55 fps).
   Paths rejected by measurement: single-buffer direct composition is
   fastest (10 ms full-scene) but visibly flickers — the panel catches
   fill-then-blend mid-paint; double-buffer + SRM-copy present costs
   ~20 ms/full-screen (SRM copies at only ~123 MB/s r+w vs ~360 MB/s for
   fill/blend); double-buffer with a vsync fence stalls into the ~71 Hz
   panel period and halves to 35 fps when compose+copy misses it.

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
   DECIDED at M7 (sprites): the op became `xform_blend(src, src_r, dst_r,
   vis, rot)` — scale via the dst/src ratio, rotation in quarter turns CCW
   (the PPA SRM limit), alpha-aware. p4 runs it as SRM-to-scratch + blend
   (the SRM engine can't blend and the blend engine can't scale); sdl is a
   nearest-neighbor inverse-mapping loop. Only transformed sprites use it;
   the 1:1 compositor path is unchanged.

   **Measured sprite limits on hardware** (ESP32-P4-Function-EV-Board,
   1024×600, IDF 5.5.1, MicroPython-driven — 2–3 property writes per
   sprite per frame included, i.e. the honest tulip-app numbers; 100-frame
   sweeps of independently moving ARGB8888 sprites):

   | scenario | 60 fps ceiling | marginal cost/sprite |
   |---|---|---|
   | 28×28 identity | ~95–100 sprites | ~130 µs |
   | 101×84 identity | ~27–30 sprites | ~410 µs |
   | ~150 px scale 1.5 + tumbling | ~7 sprites | ~1.6 ms |
   | 404×336 scale 4 + tumbling | 1–2 sprites | ~7.5 ms |

   Two regimes: small sprites are **op-limited** (~85 µs PPA floor per op;
   a moving sprite ≈ bg fill + blend + damage-forward share), so the small
   ceiling is a count, ~100/frame. Large sprites are **bandwidth-limited**:
   ~250k moving identity px/frame at 60 fps (~40% of the screen), and
   roughly half that (~130–150k px) transformed — the SRM+blend two-pass
   touches each pixel ~5× (SRM write, blend 2r+1w, bg fill, forward copy)
   against the same ~360 MB/s PSRAM budget scanout shares. Static sprites
   are free after first paint; the 512-node pool caps existence, not
   motion. Obvious M8+ optimization if transformed counts matter: cache
   the SRM output per sprite and re-blend until scale/rot changes.

   **Full-screen layer scrolling (parallax), measured.** Recomposing
   three stacked full-screen layers per frame (opaque sky blit + ARGB
   mountain blend + ground + damage-forward) is bandwidth-death:
   **19 fps** triple-buffer, 24 fps single. The winning shape is the
   `surf_layer` node + `band_shift` hal op: layers are opaque strips in
   vertically DISJOINT bands; each frame a moving band is ONE
   cross-buffer DMA2D copy from the just-presented frame at the shifted
   offset (no overlap hazard, works both directions) plus a sliver
   repaint from the strip. Present deliberately does NOT forward
   streaming bands — the next frame's shift rebuilds them from newest,
   which is what makes the arithmetic work; a band that stops moving
   repaints once (core rule). Overlay sprites are damaged (expanded by
   the shift) by the layer as it moves. Measured: the 3-layer parallax
   demo + bobbing ship runs **63–65 fps** (15.7 ms/frame) on the P4 in
   triple-buffer mode — 3.3× the naive path, over the panel rate.
   Single-buffer has no pristine source for the copy, so band_shift is
   triple-only (layers fall back to full repaint there).

   **The dirty list is a budget — movers cost one entry, not two.**
   surf_node_set_pos damages the UNION of old+new when the move wastes
   little area (per-frame movers slide a few px; the two rects are
   adjacent, and adjacent rects never coalesce — the merge rule needs
   overlap). Before this, six bullets + a ship + slivers + smears
   overflowed the 16-entry list every frame and the overflow fallback
   (degrade to one bounding union) repainted the whole world: measured
   50 -> 11 fps on the P4 the moment the parallax ship held its fire
   button. Now 32 entries, union moves, 33-43 fps under max fire. If a
   scene ever legitimately needs more independent movers than that,
   raise SURF_MAX_DIRTY before doing anything clever.

   **Overlay sprites on a fast band are the expensive thing**: they
   re-blend every frame (the band moves under them), so their cost is
   their bounding box, not their ink. Measured with the procedural-lava
   volcanoes (A8 vein masks, tint-cycled per frame): mountain-sized
   460x235 masks dragged parallax from 63 to ~29-40 fps; cropping the
   masks to the ~100x200 vein column brought it to **49-56 fps** with
   1-2 volcanoes on screen (~2 ms per visible moving overlay). Rule:
   crop overlay masks tight, and count them.

   The same band_shift drives **sprite fast pan** (`surf_sprite_set_src`
   + `surf_sprite_set_fast_pan`): a screen-sized src window panned over
   a big baked world image — the 2D-scrolling-game shape. The forest
   demo rebaked onto one 2048x1200 world image went from ~20 fps while
   walking (full recompose of ~200 nodes) to **230-250 fps compute rate**
   (4.2 ms/frame) on the P4. The exposed L-shaped slivers must be
   DISJOINT rects (the vertical sliver owns the corner) — overlapping
   slivers coalesce into a full-band repaint and silently eat the win.

   **Triple-buffer forward correctness**: back-buffer selection can skip
   a buffer when the vsync ISR lags; the skipped buffer falls 3+ frames
   behind and rect-forwarding (which only keeps one previous frame's
   damage) can never heal it — seen on hardware as a faint 1-in-3-frames
   trail under sprites moving upward. Fixed with per-buffer frame
   stamps: age 1 forwards current damage only, age 2 forwards prev +
   current (steady state), age 3+ (or a truncated prev list) full-copies.
5. **License.** DECIDED: MIT (stb is public domain/MIT), chosen at first
   public push.

6. **Fast text (textgrid) — decided by arithmetic from the M2 numbers.**
   A code editor at 1024×600/16px mono is ~2,500–5,400 glyphs, and every
   scroll damages all of them. At the PPA's measured ~85µs-per-op floor
   that is 0.2–0.46 s of blits per frame — the per-glyph atlas path can
   never scroll a full screen of text. The fix is `surf_textgrid`: a
   cols×rows grid of opaque cells (codepoint + fg + bg) that the CPU
   composes directly into the framebuffer through the optional `fb_ptr`
   hal op. Opaque cells are pure writes (no fb reads), so a full page is
   ~1.2 MB ≈ 15–20 ms predicted on the P4 (~50–60 fps page scroll),
   sub-ms for single-line edits; desktop measures 0.35 ms.

   **Measured on hardware: 45.7 ms/full page (21 fps) — the prediction
   missed two costs.** CPU stores to PSRAM go through a write-allocate
   cache (every 128 B line is read before it's written, halving
   effective write bandwidth), and the present path must msync the
   whole CPU-written band back (another ~1.2 MB of writeback). Render
   ≈ 24 ms + msync ≈ 14 ms + DMA2D damage-forward ≈ 6 ms ≈ the
   measured 46 ms. Single-cell edits remain sub-ms.

   **Scroll-blit is built and measured.** The optional `scroll_rect` hal
   op shifts pixels in place (DMA2D on the P4, memmove on desktop) and
   the textgrid's opt-in fast-scroll mode
   (`surf_textgrid_set_fast_scroll`) uses it: a line-scroll becomes one
   shift + one repainted row + a cache invalidate of the band, with the
   hal forcing a full damage-forward on scrolled frames to keep the
   triple buffers coherent. Hardware numbers for the full-screen editor:
   **triple-buffer 51 fps** (10.2 ms/tick; the two full-band DMA passes
   per frame — compose shift + buffer sync — are the remaining floor)
   and **single-buffer 91 fps** (1.9 ms/tick; no flip, no forward copy;
   the shift races scanout but is visually clean on the EK79007 —
   user-verified). Verdict: full-screen text apps (editor, terminal)
   should run the hal in single-buffer mode; mixed widget UIs keep
   triple buffering. Both are a `surf_hal_p4_cfg` flag. This is the
   sanctioned exception to "no per-pixel code outside the hal": the
   textgrid composer in `src/text/textgrid.c` is per-pixel core code,
   justified the same way the M2 asset rules were — by measurement. The
   proportional label/textinput path is unchanged and remains right for
   UI text. (On-device number pending the next hardware session; the hal
   keeps cache sync correct by msyncing the presented band at flip.)
